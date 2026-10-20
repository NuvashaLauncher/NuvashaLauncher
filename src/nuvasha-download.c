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

SoupSession *
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
 * Small helpers
 * ---------------------------------------------------------- */

/* The URL without query or fragment. Query parameters may carry
 * signatures or tokens, and error messages end up in the log. */
char *
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
gboolean
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

void
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


/* Transfer and retry state machine lives in nuvasha-download-transfer.c. */
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
   request->begin = nuvasha_download_file_cache_start;

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
