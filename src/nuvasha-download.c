/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

/* Asynchronous HTTP downloads on top of libsoup 3 and GIO.
 *
 * A file is written into a temporary file next to its destination, the
 * received bytes are hashed while they arrive, the finished file is
 * fsynced and only then renamed over the destination. A transfer that
 * fails, is cancelled or has the wrong hash therefore never touches a
 * file already in place and never leaves a temporary file behind.
 *
 * Everything runs asynchronously in the caller's thread-default main
 * context, normally the GTK main loop: no thread is used and no
 * operation blocks the loop. The one exception is fsync(2) after a
 * completed transfer, which has no asynchronous form in GIO.
 *
 * The module keeps one SoupSession and a counter of running transfers
 * for the life of the process, so all of its functions must be called
 * from the same main context. */

#include "nuvasha-download.h"
#include "nuvasha-download-private.h"
#include "nuvasha-downloads.h"

#include "nuvasha-cache.h"
#include "nuvasha-fs.h"
#include "nuvasha-task-private.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Attempts for a transport error, an HTTP 5xx or an HTTP 429. */
#define MAX_ATTEMPTS 3

/* Pause before a new attempt when the server asked for nothing. */
#define RETRY_DELAY_MS 100

/* Longest pause taken from a Retry-After header, so a download queue
 * cannot stall the launcher for minutes. */
#define MAX_RETRY_AFTER_MS (30 * 1000)

/* Transfers running at the same time. */

/* Bytes taken from the network in one step. */
#define CHUNK_SIZE (64 * 1024)

/* API documents are parsed in the main context. Refuse unexpectedly huge
 * responses before they can monopolize memory or freeze the UI. */

GQuark
nuvasha_download_error_quark (void)
{
  return g_quark_from_static_string ("nuvasha-download-error");
}

/* ---------------------------------------------------------- *
 * State shared by every request
 * ---------------------------------------------------------- */

/* One session for the whole launcher: a session per download would
 * throw away the connection pool. It lives until the process exits. */


guint
nuvasha_download_service_active (const NuvashaDownloadService *service)
{
  return service == NULL ? 0 : service->interactive_running +
                                service->bulk_running;
}

guint
nuvasha_download_service_queued_interactive (const NuvashaDownloadService *service)
{
  return service == NULL ? 0 :
    g_queue_get_length ((GQueue *) &service->interactive_waiting);
}

guint
nuvasha_download_service_queued_bulk (const NuvashaDownloadService *service)
{
  return service == NULL ? 0 :
    g_queue_get_length ((GQueue *) &service->bulk_waiting);
}

NuvashaDownloadService *
nuvasha_download_service_new (void)
{
  NuvashaDownloadService *service = g_new0 (NuvashaDownloadService, 1);
  g_queue_init (&service->interactive_waiting);
  g_queue_init (&service->bulk_waiting);
  service->prefer_interactive = TRUE;
  return service;
}

void
nuvasha_download_service_free (NuvashaDownloadService *owned_service)
{
  if (owned_service == NULL)
    return;
  nuvasha_download_service_cancel_queued (owned_service);
  if (owned_service->interactive_running != 0 ||
      owned_service->bulk_running != 0)
    {
      /* Requests borrow the scheduler.  Defer destruction until the last
       * request releases its slot instead of leaking the service forever. */
      owned_service->destroy_requested = TRUE;
      return;
    }
  g_clear_object (&owned_service->session);
  g_clear_object (&owned_service->cache_settings);
  g_free (owned_service);
}

void
service_destroy_if_requested (NuvashaDownloadService *service)
{
  if (!service->destroy_requested ||
      service->interactive_running != 0 || service->bulk_running != 0)
    return;

  g_clear_object (&service->session);
  g_clear_object (&service->cache_settings);
  g_free (service);
}

static SoupSession *
get_session (NuvashaDownloadService *service)
{
  if (service->session == NULL)
     /* Set connection limits when constructing the session so they match
        the transfer scheduler. */
    service->session = g_object_new (SOUP_TYPE_SESSION, "user-agent",
                            "nuvasha (Nuvasha Launcher)", "timeout",
                            (guint) 60, "max-conns", (gint) MAX_CONCURRENT,
                            "max-conns-per-host", (gint) MAX_CONCURRENT, NULL);

  return service->session;
}

/* ---------------------------------------------------------- *
 * One request
 * ---------------------------------------------------------- */

static void start_attempt (DownloadRequest *request);
static void file_request_start (DownloadRequest *request);
static void read_chunk_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void write_chunk_ready (GObject *source, GAsyncResult *result,
                               gpointer user_data);
static void source_closed_ready (GObject *source, GAsyncResult *result,
                                 gpointer user_data);
static void attempt_next (DownloadRequest *request);
static void attempt_failed (DownloadRequest *request, GError *error);
static gboolean retry_when_due (gpointer user_data);
static gboolean retry_cancelled (gpointer user_data);
gboolean queued_cancelled (gpointer user_data);
static void send_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data);
void nuvasha_download_json_stream_ready (GObject *source, GAsyncResult *result,
                               gpointer user_data);
void nuvasha_download_json_process_body (DownloadRequest *request, GBytes *body);
static void cache_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void json_cache_start (DownloadRequest *request);
static void cache_load_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
void nuvasha_download_request_succeed_json (DownloadRequest *request,
                                        JsonParser *parser);
static void global_cache_hash_ready (GObject *source, GAsyncResult *result,
                                     gpointer user_data);
static void global_cache_copy_ready (GObject *source, GAsyncResult *result,
                                      gpointer user_data);
static void global_cache_copy_hash_ready (GObject *source,
                                           GAsyncResult *result,
                                           gpointer user_data);
static void global_cache_directory_ready (GObject *source,
                                          GAsyncResult *result,
                                          gpointer user_data);
static void global_cache_files_ready (GObject *source, GAsyncResult *result,
                                      gpointer user_data);
GError *nuvasha_download_cancelled_error (void);
static gboolean request_was_cancelled (DownloadRequest *request, GError *error);
void nuvasha_download_request_fail (DownloadRequest *request, GError *error);
static void request_succeed (DownloadRequest *request);

/* ---------------------------------------------------------- *
 * Small helpers
 * ---------------------------------------------------------- */

/* The URL without query or fragment. Query parameters may carry
 * signatures or tokens, and error messages end up in the log. */
static char *
url_for_message (const char *url)
{
  const char *end = strpbrk (url, "?#");

  return end == NULL ? g_strdup (url) : g_strndup (url, end - url);
}

GError *
nuvasha_download_cancelled_error (void)
{
  return g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_CANCELLED,
                      "the download was cancelled");
}

/* Cancellation wins over whatever the transport reported, so a
 * cancelled transfer is never reported as a network error. */
static gboolean
request_was_cancelled (DownloadRequest *request, GError *error)
{
  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    return TRUE;

  return error != NULL &&
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* TRUE when libsoup accepts the string as the URL of a request. */
static gboolean
usable_url (const char *url)
{
  g_autoptr (SoupMessage) message = soup_message_new (SOUP_METHOD_GET, url);

  return message != NULL;
}

gint
compare_waiting_requests (gconstpointer left, gconstpointer right,
                          gpointer user_data G_GNUC_UNUSED)
{
  const DownloadRequest *a = left;
  const DownloadRequest *b = right;

  if (a->priority == b->priority)
    return 0;
  return a->priority > b->priority ? -1 : 1;
}



/* ---------------------------------------------------------- *
 * Ending a request
 * ---------------------------------------------------------- */

static void
request_free (DownloadRequest *request)
{
  if (request->scope != NULL)
    {
      nuvasha_task_scope_set_state (request->scope, nuvasha_TASK_STATE_CLEANING);
      g_debug ("download operation=%" G_GUINT64_FORMAT " cleaning",
               nuvasha_task_scope_get_id (request->scope));
    }
  if (request->retry_source != 0)
    nuvasha_task_scope_remove_source (request->scope, request->retry_source);

  /* Through the scope, which is where this source was registered: removing
     it by hand would leave its id behind, and a scope that later tears that
     id down removes whatever source has been given the same id since. */
  if (request->retry_cancel_source != 0)
    nuvasha_task_scope_remove_source (request->scope,
                                  request->retry_cancel_source);

  if (request->queue_cancel_source != 0)
    nuvasha_task_scope_remove_source (request->scope,
                                  request->queue_cancel_source);

  nuvasha_download_close_sink (request);
  g_clear_pointer (&request->chunk, g_bytes_unref);
  g_clear_pointer (&request->json_body, g_byte_array_unref);
  g_clear_pointer (&request->checksum, g_checksum_free);
  g_clear_pointer (&request->tmp_path, g_free);
  g_clear_pointer (&request->tmp_name, g_free);
  request->scope = NULL;
  if (request->parent_fd >= 0)
    close (request->parent_fd);
  g_clear_object (&request->source);
  g_clear_object (&request->message);
  g_clear_object (&request->task);
  g_clear_error (&request->pending_error);
  g_free (request->url);
  g_free (request->dest_path);
  g_free (request->expected_hash);
  g_free (request->body);
  g_free (request->user_agent);
  g_free (request->cache_path);
  g_free (request->global_cache_path);
  g_free (request->stale_json);
  g_free (request->cache_etag);
  g_free (request->response_etag);
  g_clear_object (&request->pending_parser);
  g_free (request->pending_cache_data);
  g_free (request);
}

/* Every answer is given in the same order: the request is torn down first,
   the task is completed second. The caller's callback is free to drop the
   last reference on the scope, and request_free() still has to reach the
   scope to take this request's sources off it. */
void
nuvasha_download_request_fail (DownloadRequest *request, GError *error)
{
  NuvashaTaskScope *scope = request->scope;
  gboolean cancelled = g_error_matches (error, nuvasha_DOWNLOAD_ERROR,
                                        nuvasha_DOWNLOAD_ERROR_CANCELLED);

  nuvasha_task_scope_set_state (scope, cancelled ? nuvasha_TASK_STATE_CANCELLED
                                               : nuvasha_TASK_STATE_FAILED);
  g_debug ("download operation=%" G_GUINT64_FORMAT " %s", 
           nuvasha_task_scope_get_id (scope), cancelled ? "cancelled" : "failed");
  nuvasha_download_discard_temporary (request);
  nuvasha_download_release_slot (request);
  request_free (request);
  nuvasha_task_scope_return_error (scope, error);
}

static void
request_succeed (DownloadRequest *request)
{
  NuvashaTaskScope *scope = request->scope;
  g_debug ("download operation=%" G_GUINT64_FORMAT " completed",
           nuvasha_task_scope_get_id (scope));

  nuvasha_download_release_slot (request);
  request_free (request);
  nuvasha_task_scope_return_boolean (scope, TRUE);
}

void
nuvasha_download_request_succeed_json (DownloadRequest *request, JsonParser *parser)
{
  NuvashaTaskScope *scope = request->scope;
  g_debug ("download operation=%" G_GUINT64_FORMAT " completed",
           nuvasha_task_scope_get_id (scope));

  nuvasha_download_release_slot (request);
  request_free (request);
  nuvasha_task_scope_return_pointer (scope, parser, g_object_unref);
}


/* ---------------------------------------------------------- *
 * Checking the cache before downloading
 * ---------------------------------------------------------- */

static void
file_request_start (DownloadRequest *request)
{
  if (request->expected_hash == NULL)
    {
      /* Without an expected hash there is nothing an existing file
         could be compared against. The slot is already held. */
      start_attempt (request);
      return;
    }

  g_autoptr (GFile) destination = g_file_new_for_path (request->dest_path);

  nuvasha_task_scope_release_when_done (
    nuvasha_download_hash_file_async (destination, request->hash_type, cache_ready,
                                  request),
    g_task_get_cancellable (request->task));
}

static void
global_cache_copy (DownloadRequest *request)
{
  g_autoptr (GFile) source = g_file_new_for_path (request->dest_path);
  g_autoptr (GFile) destination =
    g_file_new_for_path (request->global_cache_path);
  nuvasha_fs_copy_async (source, destination, G_FILE_COPY_OVERWRITE,
                     G_PRIORITY_DEFAULT, g_task_get_cancellable (request->task),
                     NULL, NULL, global_cache_copy_ready, request);
}

static void
global_cache_directory_ready (GObject *source, GAsyncResult *result,
                               gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = nuvasha_cache_get_files_directory ();
  g_autoptr (GFile) files = g_file_new_for_path (directory);

  if (!nuvasha_fs_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
          return;
        }
      g_debug ("the global file cache directory was not created: %s",
               error->message);
      request_succeed (request);
      return;
    }

  nuvasha_fs_make_directory_async (files, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               global_cache_files_ready, request);
}

static void
global_cache_files_ready (GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvasha_fs_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
          return;
        }
      g_debug ("the global file cache directory was not created: %s",
               error->message);
      request_succeed (request);
      return;
    }

  global_cache_copy (request);
}

static void
global_cache_copy_ready (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvasha_fs_copy_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("the global file cache was not written: %s", error->message);

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return;
    }

  /* The source was hashed before the asynchronous copy.  Hash the bytes
   * actually adopted at the destination as well, so a replacement during
   * the copy cannot satisfy the request. */
  {
    g_autoptr (GFile) destination = g_file_new_for_path (request->dest_path);
    nuvasha_task_scope_release_when_done (
      nuvasha_download_hash_file_async (destination, request->hash_type,
                                    global_cache_copy_hash_ready, request),
      g_task_get_cancellable (request->task));
  }
}

static void
global_cache_copy_hash_ready (GObject *source G_GNUC_UNUSED,
                              GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = nuvasha_download_file_hash_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return;
    }

  if (digest != NULL &&
      g_ascii_strcasecmp (digest, request->expected_hash) == 0)
    request_succeed (request);
  else
    start_attempt (request);
}

static void
global_cache_hash_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                         gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = nuvasha_download_file_hash_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return;
    }

  if (digest != NULL &&
      g_ascii_strcasecmp (digest, request->expected_hash) == 0)
    {
      g_autoptr (GFile) cache_file =
        g_file_new_for_path (request->global_cache_path);
      g_autoptr (GFile) destination = g_file_new_for_path (request->dest_path);

       nuvasha_fs_copy_async (cache_file, destination, G_FILE_COPY_OVERWRITE,
                         G_PRIORITY_DEFAULT,
                         g_task_get_cancellable (request->task), NULL, NULL,
                         global_cache_copy_ready, request);
      return;
    }

  start_attempt (request);
}

static void
json_cache_start (DownloadRequest *request)
{
  NuvashaSettings *settings = nuvasha_download_get_cache_settings (request);

  request->api_cache = nuvasha_settings_get_cache_modrinth_api_responses (settings);
  request->cache_expiration = nuvasha_settings_get_cache_expiration (settings);

  if (!request->api_cache)
    {
      start_attempt (request);
      return;
    }

  request->cache_path = nuvasha_download_api_cache_path (request);
  {
    g_autoptr (GFile) file = g_file_new_for_path (request->cache_path);

    g_file_load_contents_async (file, g_task_get_cancellable (request->task),
                                cache_load_ready, request);
  }
}

static void
cache_load_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *response = NULL;
  gint64 created = 0;
  gsize length = 0;

  if (!g_file_load_contents_finish (G_FILE (source), result, &contents,
                                    &length, NULL, &error))
    {
      if (request_was_cancelled (request, error))
        {
          nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
          return;
        }

      start_attempt (request);
      return;
    }

    {
      g_autofree char *etag = NULL;

      parser = nuvasha_download_cache_parser (contents, length, &response, &etag, &created);
      g_clear_pointer (&request->cache_etag, g_free);
      request->cache_etag = g_steal_pointer (&etag);
    }
  if (parser == NULL)
    {
      start_attempt (request);
      return;
    }

  if (g_get_real_time () / G_USEC_PER_SEC - created <=
      (gint64) request->cache_expiration)
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
          return;
        }

      nuvasha_download_request_succeed_json (request, g_steal_pointer (&parser));
      return;
    }

  /* Keep only a valid JSON response. It may still be useful if the
   * refresh cannot reach Modrinth. */
  request->stale_json = g_steal_pointer (&response);
  start_attempt (request);
}

static void
cache_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
    g_autofree char *digest = nuvasha_download_hash_file_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return;
    }

  if (digest != NULL &&
      g_ascii_strcasecmp (digest, request->expected_hash) == 0)
    {
      /* The file on disk already holds exactly what was asked for. */
      request_succeed (request);
      return;
    }

  /* Missing, unreadable or stale: fetch it again. The slot is already
     held, so this goes straight to the transfer. */
  g_clear_error (&error);
  request->global_cache_path = nuvasha_download_global_cache_path (request->expected_hash);
  {
    g_autoptr (GFile) global =
      g_file_new_for_path (request->global_cache_path);
    nuvasha_task_scope_release_when_done (
      nuvasha_download_hash_file_async (global, request->hash_type,
                                    global_cache_hash_ready, request),
      g_task_get_cancellable (request->task));
  }
}


/* ---------------------------------------------------------- *
 * Reading the response status
 * ---------------------------------------------------------- */

/* How long the server asked us to wait, in milliseconds, or -1 when it
 * did not say. A delay longer than MAX_RETRY_AFTER_MS is ignored. */
static gint64
retry_after_ms (DownloadRequest *request)
{
  SoupMessageHeaders *headers =
    soup_message_get_response_headers (request->message);
  const char *value = soup_message_headers_get_one (headers, "Retry-After");
  char *end = NULL;
  gint64 seconds;
  g_autoptr (GDateTime) deadline = NULL;
  g_autoptr (GDateTime) now = NULL;

  if (value == NULL)
    return -1;

  seconds = g_ascii_strtoll (value, &end, 10);

  if (end != value && *end == '\0')
    {
      seconds = CLAMP (seconds, 0, MAX_RETRY_AFTER_MS / 1000);
      return seconds * 1000;
    }

  deadline = soup_date_time_new_from_http_string (value);

  if (deadline == NULL)
    return -1;

  now = g_date_time_new_now_utc ();

  return CLAMP (g_date_time_difference (deadline, now) / 1000, (gint64) 0,
                (gint64) MAX_RETRY_AFTER_MS);
}

/* NULL when the response is a success status, otherwise the error this
 * attempt ends with. HTTP 429 gets its own code so the caller can see
 * that the server asked to slow down. */
GError *
check_status (DownloadRequest *request)
{
  guint status = soup_message_get_status (request->message);
  const char *reason = NULL;
  g_autofree char *url = NULL;

  if (SOUP_STATUS_IS_SUCCESSFUL (status))
    return NULL;

  request->http_status = status;
  url = url_for_message (request->url);

  if (status == 429)
    return g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_RATE_LIMITED,
                        "HTTP 429 too many requests for %s", url);

  reason = soup_message_get_reason_phrase (request->message);

  return g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_HTTP, "HTTP %u %s for %s",
                      status, reason != NULL ? reason : "Error", url);
}


/* ---------------------------------------------------------- *
 * Transferring the body
 * ---------------------------------------------------------- */

static void
report_progress (DownloadRequest *request)
{
  if (request->progress != NULL)
    request->progress (request->done, request->total, request->user_data);
}

void read_next_chunk (DownloadRequest *request);
void end_attempt (DownloadRequest *request, GError *error);

static void
send_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  GInputStream *stream =
    soup_session_send_finish (SOUP_SESSION (source), result, &error);
  GError *status_error = NULL;
  SoupMessageHeaders *headers = NULL;
  const char *length = NULL;

  if (stream == NULL)
    {
      /* Nothing has been written yet. */
      end_attempt (request, g_steal_pointer (&error));
      return;
    }

  request->source = stream;
  status_error = check_status (request);

  if (status_error != NULL)
    {
      end_attempt (request, status_error);
      return;
    }

  headers = soup_message_get_response_headers (request->message);
  length = soup_message_headers_get_one (headers, "Content-Length");
  request->done = 0;
  request->total = length != NULL
                     ? (goffset) soup_message_headers_get_content_length (
                         headers)
                     : -1;

  report_progress (request);
  read_next_chunk (request);
}

void
read_next_chunk (DownloadRequest *request)
{
  g_input_stream_read_bytes_async (request->source, CHUNK_SIZE,
                                   G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (request->task),
                                   read_chunk_ready, request);
}

static void
read_chunk_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (bytes == NULL)
    {
      end_attempt (request, g_steal_pointer (&error));
      return;
    }

  if (g_bytes_get_size (bytes) == 0)
    {
      /* End of the response body. */
      if (request->json)
        {
          g_autoptr (GBytes) body =
            g_byte_array_free_to_bytes (g_steal_pointer (&request->json_body));
          nuvasha_download_json_process_body (request, body);
        }
      else
        end_attempt (request, NULL);
      return;
    }

  gsize size = 0;
  const char *data = NULL;

  request->chunk = g_steal_pointer (&bytes);
  data = g_bytes_get_data (request->chunk, &size);

  if (request->json)
    {
      if (request->json_body->len + size > MAX_JSON_RESPONSE_SIZE)
        {
          end_attempt (request,
                       g_error_new_literal (nuvasha_DOWNLOAD_ERROR,
                                            nuvasha_DOWNLOAD_ERROR_IO,
                                            "the JSON response is too large"));
          return;
        }
      g_byte_array_append (request->json_body, (const guint8 *) data, size);
      g_clear_pointer (&request->chunk, g_bytes_unref);
      read_next_chunk (request);
      return;
    }

  g_output_stream_write_all_async (request->sink, data, size,
                                   G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (request->task),
                                   write_chunk_ready, request);
}

static void
write_chunk_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  gsize written = 0;

  if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (source), result,
                                         &written, &error))
    {
      g_clear_pointer (&request->chunk, g_bytes_unref);

      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          /* The bytes arrived, putting them on disk did not work. */
          GError *io = g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_IO,
                                    "cannot write to \"%s\": %s",
                                    request->tmp_path, error->message);

          g_clear_error (&error);
          error = io;
        }

      end_attempt (request, g_steal_pointer (&error));
      return;
    }

  if (request->checksum != NULL)
    g_checksum_update (request->checksum,
                       g_bytes_get_data (request->chunk, NULL), written);

  request->done += written;
  g_clear_pointer (&request->chunk, g_bytes_unref);

  report_progress (request);
  read_next_chunk (request);
}

/* The whole body arrived: verify it, get it onto the disk and move it
 * into place. Only reached by a file request; a JSON request answers
 * from json_ready() instead. */
static void
verify_and_install (DownloadRequest *request)
{
  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return;
    }

  if (request->expected_hash != NULL &&
      g_ascii_strcasecmp (g_checksum_get_string (request->checksum),
                          request->expected_hash) != 0)
    {
      g_autofree char *got =
        g_strdup (g_checksum_get_string (request->checksum));

      attempt_failed (request,
                      g_error_new (nuvasha_DOWNLOAD_ERROR,
                                   nuvasha_DOWNLOAD_ERROR_CHECKSUM,
                                   "checksum of \"%s\" does not match: "
                                   "expected %s, got %s",
                                   request->dest_path, request->expected_hash,
                                   got));
      return;
    }

  /* Every byte is in the kernel: flush the file before it gets its
   * final name. fsync() has no asynchronous form in GIO. */
  if (fsync (request->fd) != 0)
    {
      int errsv = errno;

      attempt_failed (request,
                      g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_IO,
                                   "cannot flush \"%s\": %s",
                                   request->tmp_path, g_strerror (errsv)));
      return;
    }

  nuvasha_download_close_sink (request);

  g_autofree char *destination_name = g_path_get_basename (request->dest_path);
  g_autoptr (GError) rename_error = NULL;
  if (!nuvasha_fs_rename_at (request->parent_fd, request->tmp_name,
                         destination_name, &rename_error))
    {
      attempt_failed (request,
                      g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_IO,
                                   "cannot rename \"%s\" to \"%s\": %s",
                                   request->tmp_path, request->dest_path,
                                   rename_error->message));
      return;
    }

  g_clear_pointer (&request->tmp_path, g_free);
  if (request->expected_hash != NULL)
    {
      g_autofree char *directory = nuvasha_cache_get_files_directory ();
      g_autoptr (GFile) directory_file = g_file_new_for_path (directory);
      g_autoptr (GFile) parent = g_file_get_parent (directory_file);

      if (request->global_cache_path == NULL)
        request->global_cache_path = nuvasha_download_global_cache_path (request->expected_hash);
      nuvasha_fs_make_directory_async (parent, G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (request->task),
                                   global_cache_directory_ready, request);
      return;
    }
  request_succeed (request);
}

/* Ends one attempt. A NULL @error means that a file body arrived
 * complete, which only a file request reports. The response stream is
 * always closed first so the connection goes back to the session. */
void
end_attempt (DownloadRequest *request, GError *error)
{
  g_assert (request->pending_error == NULL);

  request->pending_error = error;

  if (request->source == NULL)
    {
      attempt_next (request);
      return;
    }

  g_input_stream_close_async (request->source, G_PRIORITY_DEFAULT, NULL,
                              source_closed_ready, request);
}

static void
source_closed_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;

  g_input_stream_close_finish (G_INPUT_STREAM (source), result, NULL);
  g_clear_object (&request->source);

  attempt_next (request);
}

static void
attempt_next (DownloadRequest *request)
{
  g_autoptr (GError) error = g_steal_pointer (&request->pending_error);

  if (error != NULL)
    {
      attempt_failed (request, g_steal_pointer (&error));
      return;
    }

  verify_and_install (request);
}


/* ---------------------------------------------------------- *
 * Retrying an attempt
 * ---------------------------------------------------------- */

/* Decides whether the attempt is worth repeating and schedules it, or
 * ends the request with the error. Consumes @error. */
static void
attempt_failed (DownloadRequest *request, GError *error)
{
  gboolean retry = FALSE;
  guint delay_ms = RETRY_DELAY_MS;

  if (request_was_cancelled (request, error))
    {
      g_clear_error (&error);
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return;
    }

  if (error->domain != nuvasha_DOWNLOAD_ERROR)
    {
      /* The transport reports in its own domains: refused connection,
         name resolution, timeout, broken pipe. The caller sees one. */
      GError *wrapped =
        g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_NETWORK, "%s",
                     error->message);

      g_error_free (error);
      error = wrapped;
    }

  if (request->stale_json != NULL &&
      g_error_matches (error, nuvasha_DOWNLOAD_ERROR,
                       nuvasha_DOWNLOAD_ERROR_NETWORK))
    {
      g_autoptr (JsonParser) parser = json_parser_new ();

      if (json_parser_load_from_data (parser, request->stale_json, -1, NULL) &&
          json_parser_get_root (parser) != NULL)
        {
          g_clear_error (&error);
          nuvasha_download_request_succeed_json (request, g_steal_pointer (&parser));
          return;
        }
    }

  if (g_error_matches (error, nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_IO))
    retry = FALSE;
  else if (g_error_matches (error, nuvasha_DOWNLOAD_ERROR,
                            nuvasha_DOWNLOAD_ERROR_CHECKSUM))
    {
      /* Wrong bytes: one fresh transfer, and no more than that. */
      retry = !request->hash_retried && request->attempts < MAX_ATTEMPTS;
      request->hash_retried = retry;
    }
  else if (g_error_matches (error, nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_HTTP))
    retry = request->http_status >= 500 && request->attempts < MAX_ATTEMPTS;
  else if (g_error_matches (error, nuvasha_DOWNLOAD_ERROR,
                            nuvasha_DOWNLOAD_ERROR_RATE_LIMITED))
    {
      retry = request->attempts < MAX_ATTEMPTS;

      if (retry)
        {
          gint64 asked = retry_after_ms (request);

          if (asked >= 0)
            delay_ms = (guint) asked;
        }
    }
  else
    retry = request->attempts < MAX_ATTEMPTS;

  if (retry)
    {
      nuvasha_download_discard_temporary (request);
      g_clear_error (&error);
      nuvasha_task_scope_set_state (request->scope, nuvasha_TASK_STATE_RETRYING);
      g_debug ("download operation=%" G_GUINT64_FORMAT " retry attempt=%u",
               nuvasha_task_scope_get_id (request->scope), request->attempts + 1);
      request->retry_source = g_timeout_add (delay_ms, retry_when_due, request);
      nuvasha_task_scope_track_source (request->scope, request->retry_source);
      request->retry_cancel_source =
        nuvasha_cancellable_watch (g_task_get_cancellable (request->task),
                               retry_cancelled, request, NULL);
      nuvasha_task_scope_track_source (request->scope,
                                   request->retry_cancel_source);
      return;
    }

  nuvasha_download_request_fail (request, error);
}

static gboolean
retry_when_due (gpointer user_data)
{
  DownloadRequest *request = user_data;

  nuvasha_task_scope_forget_source (request->scope, request->retry_source);
  request->retry_source = 0;
  if (request->retry_cancel_source != 0)
    {
      nuvasha_task_scope_remove_source (request->scope,
                                    request->retry_cancel_source);
      request->retry_cancel_source = 0;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return G_SOURCE_REMOVE;
    }

  start_attempt (request);

  return G_SOURCE_REMOVE;
}

static gboolean
retry_cancelled (gpointer user_data)
{
  DownloadRequest *request = user_data;

  /* Off the scope before the answer, so nothing is left holding an id for
     a source this one is about to destroy. */
  if (request->retry_cancel_source != 0)
    {
      nuvasha_task_scope_remove_source (request->scope,
                                    request->retry_cancel_source);
      request->retry_cancel_source = 0;
    }

  nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
  return G_SOURCE_REMOVE;
}

/* Cancelled while waiting for a transfer slot. The watch is torn down by
   returning G_SOURCE_REMOVE, which also means this cannot be asked twice. */
gboolean
queued_cancelled (gpointer user_data)
{
  DownloadRequest *request = user_data;

  NuvashaDownloadService *service = request->service;

  if (g_queue_remove (request->priority >= DOWNLOAD_PRIORITY_INTERACTIVE
                        ? &service->interactive_waiting
                        : &service->bulk_waiting,
                      request))
    nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());

  return G_SOURCE_REMOVE;
}


/* ---------------------------------------------------------- *
 * Starting one attempt
 * ---------------------------------------------------------- */

static void
start_attempt (DownloadRequest *request)
{
  GCancellable *cancellable = g_task_get_cancellable (request->task);
  g_autoptr (GError) error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      nuvasha_download_request_fail (request, nuvasha_download_cancelled_error ());
      return;
    }

  g_clear_object (&request->message);
  g_clear_object (&request->source);
  g_clear_pointer (&request->chunk, g_bytes_unref);
  g_clear_pointer (&request->checksum, g_checksum_free);
  g_clear_error (&request->pending_error);
  request->done = 0;
  request->total = -1;
  request->http_status = 0;
  request->attempts++;

  request->message = soup_message_new (
      request->body != NULL ? SOUP_METHOD_POST : SOUP_METHOD_GET,
      request->url);

  if (request->message == NULL)
    {
      g_autofree char *url = url_for_message (request->url);

      nuvasha_download_request_fail (request,
                    g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_NETWORK,
                                 "\"%s\" is not a usable download url", url));
      return;
    }

  if (request->body != NULL)
    soup_message_set_request_body_from_bytes (
        request->message, "application/json",
        g_bytes_new (request->body, strlen (request->body)));

  if (request->user_agent != NULL)
    soup_message_headers_replace (
        soup_message_get_request_headers (request->message), "User-Agent",
         request->user_agent);

  if (request->json && request->cache_etag != NULL)
    soup_message_headers_replace (
      soup_message_get_request_headers (request->message), "If-None-Match",
      request->cache_etag);


  if (request->json)
    {
      request->json_body = g_byte_array_new ();
      soup_session_send_async (get_session (request->service), request->message,
                               G_PRIORITY_DEFAULT, cancellable,
                               nuvasha_download_json_stream_ready, request);
      return;
    }

  if (!nuvasha_download_open_temporary (request, &error))
    {
      nuvasha_download_request_fail (request, g_steal_pointer (&error));
      return;
    }

  if (request->expected_hash != NULL)
    request->checksum = g_checksum_new (request->hash_type);

  soup_session_send_async (get_session (request->service), request->message,
                           G_PRIORITY_DEFAULT, cancellable, send_ready,
                           request);
}




NuvashaTaskScope *
nuvasha_download_file_async (NuvashaDownloadService *service, const char *url,
                         const char *dest_path,
                         const char *expected_hash,
                         GChecksumType hash_type,
                         NuvashaDownloadProgressFunc progress,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
  g_return_val_if_fail (url != NULL, NULL);
  g_return_val_if_fail (dest_path != NULL, NULL);
  g_return_val_if_fail (g_path_is_absolute (dest_path), NULL);

  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);

  /* Cancellation is reported by this module, in its own error domain,
     so GTask must not replace the error with G_IO_ERROR_CANCELLED. */
  g_task_set_check_cancellable (nuvasha_task_scope_task (scope), FALSE);

  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);

      nuvasha_task_scope_return_error_deferred (
        scope,
        g_error_new (nuvasha_DOWNLOAD_ERROR,
                     nuvasha_DOWNLOAD_ERROR_NETWORK,
                     "\"%s\" is not a usable download url", shown));
      return g_steal_pointer (&scope);
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;

  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->scope = scope;
   request->service = service;
   request->task = g_object_ref (nuvasha_task_scope_task (scope));
  request->url = g_strdup (url);
  request->dest_path = g_strdup (dest_path);
  request->expected_hash = g_strdup (expected_hash);
  request->hash_type = hash_type;
  request->progress = progress;
  request->user_data = user_data;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_BULK;
  request->begin = file_request_start;

  nuvasha_download_start_transfer (request);

  return g_steal_pointer (&scope);
}

gboolean
nuvasha_download_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

NuvashaTaskScope *
nuvasha_download_json_async (NuvashaDownloadService *service, const char *url,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
  g_return_val_if_fail (url != NULL, NULL);

  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);

  /* See nuvasha_download_file_async(). */
  g_task_set_check_cancellable (nuvasha_task_scope_task (scope), FALSE);

  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);

      nuvasha_task_scope_return_error_deferred (
        scope,
        g_error_new (nuvasha_DOWNLOAD_ERROR,
                     nuvasha_DOWNLOAD_ERROR_NETWORK,
                     "\"%s\" is not a usable download url", shown));
      return g_steal_pointer (&scope);
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;

  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->scope = scope;
   request->service = service;
   request->task = g_object_ref (nuvasha_task_scope_task (scope));
  request->url = g_strdup (url);
  request->json = TRUE;
  request->user_data = user_data;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_INTERACTIVE;
  request->begin = start_attempt;

  nuvasha_download_start_transfer (request);

  return g_steal_pointer (&scope);
}

void
nuvasha_download_set_cache_settings (NuvashaDownloadService *service,
                                  NuvashaSettings *settings)
{
  g_return_if_fail (settings == NULL || nuvasha_IS_SETTINGS (settings));

  g_set_object (&service->cache_settings, settings);
}

JsonParser *
nuvasha_download_json_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

NuvashaTaskScope *
nuvasha_download_json_cached_async (NuvashaDownloadService *service, const char *url,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_return_val_if_fail (url != NULL, NULL);
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);

  g_task_set_check_cancellable (nuvasha_task_scope_task (scope), FALSE);
  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);
      nuvasha_task_scope_return_error_deferred (
        scope,
        g_error_new (nuvasha_DOWNLOAD_ERROR,
                     nuvasha_DOWNLOAD_ERROR_NETWORK,
                     "\"%s\" is not a usable download url", shown));
      return g_steal_pointer (&scope);
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;
  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->scope = scope;
   request->service = service;
   request->task = g_object_ref (nuvasha_task_scope_task (scope));
  request->url = g_strdup (url);
  request->json = TRUE;
  request->api_cache = TRUE;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_INTERACTIVE;
  request->begin = json_cache_start;
  nuvasha_download_start_transfer (request);

  return g_steal_pointer (&scope);
}

NuvashaTaskScope *
nuvasha_download_json_post_cached_async (NuvashaDownloadService *service,
                                     const char *url, const char *body,
                                     const char *user_agent,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  g_return_val_if_fail (url != NULL, NULL);
  g_return_val_if_fail (body != NULL, NULL);
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);

  g_task_set_check_cancellable (nuvasha_task_scope_task (scope), FALSE);
  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);
      nuvasha_task_scope_return_error_deferred (
        scope,
        g_error_new (nuvasha_DOWNLOAD_ERROR, nuvasha_DOWNLOAD_ERROR_NETWORK,
                     "\"%s\" is not a usable request url", shown));
      return g_steal_pointer (&scope);
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;
  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->scope = scope;
   request->service = service;
   request->task = g_object_ref (nuvasha_task_scope_task (scope));
  request->url = g_strdup (url);
  request->body = g_strdup (body);
  request->user_agent = g_strdup (user_agent);
  request->json = TRUE;
  request->api_cache = TRUE;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_INTERACTIVE;
  request->begin = json_cache_start;
  nuvasha_download_start_transfer (request);

  return g_steal_pointer (&scope);
}
