/* HTTP streaming, verification, and retry handling. */
#include "nuvasha-download-private.h"
#include "nuvasha-cache.h"
#include "nuvasha-fs.h"
#include "nuvasha-task-private.h"
#include "nuvasha-downloads.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gunixoutputstream.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ATTEMPTS 3
#define RETRY_DELAY_MS 100
#define MAX_RETRY_AFTER_MS (30 * 1000)
#define CHUNK_SIZE (64 * 1024)

static void read_chunk_ready (GObject *, GAsyncResult *, gpointer);
static void write_chunk_ready (GObject *, GAsyncResult *, gpointer);
static void source_closed_ready (GObject *, GAsyncResult *, gpointer);
static void attempt_next (DownloadRequest *);
static void attempt_failed (DownloadRequest *, GError *);
static gboolean retry_when_due (gpointer);
static gboolean retry_cancelled (gpointer);

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

void
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



