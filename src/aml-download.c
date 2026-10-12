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

#include "aml-download.h"

#include "aml-cache.h"
#include "aml-fs.h"
#include "aml-task-private.h"

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
#define MAX_CONCURRENT 8
#define MAX_BULK_CONCURRENT 6

/* Bytes taken from the network in one step. */
#define CHUNK_SIZE (64 * 1024)
#define DOWNLOAD_PRIORITY_BULK 0
#define DOWNLOAD_PRIORITY_INTERACTIVE 100

/* API documents are parsed in the main context. Refuse unexpectedly huge
 * responses before they can monopolize memory or freeze the UI. */
#define MAX_JSON_RESPONSE_SIZE (64ULL * 1024ULL * 1024ULL)

/* Name template of the temporary file. It is created in the directory
 * of the destination, so the final rename never crosses a file
 * system. */
#define TEMP_TEMPLATE ".aml-XXXXXX"

GQuark
aml_download_error_quark (void)
{
  return g_quark_from_static_string ("aml-download-error");
}

/* ---------------------------------------------------------- *
 * State shared by every request
 * ---------------------------------------------------------- */

/* One session for the whole launcher: a session per download would
 * throw away the connection pool. It lives until the process exits. */
struct _AmlDownloadService {
  SoupSession *session;
  guint interactive_running;
  guint bulk_running;
  GQueue interactive_waiting;
  GQueue bulk_waiting;
  AmlSettings *cache_settings;
};

static AmlDownloadService *service;

static void
download_return_new_error (GTask       *task,
                           GQuark       domain,
                           gint         code,
                           const char  *format,
                           ...)
{
  AmlTaskScope *scope = g_object_get_data (G_OBJECT (task),
                                           "aml-task-scope");
  va_list args;
  va_start (args, format);
  GError *error = g_error_new_valist (domain, code, format, args);
  va_end (args);
  aml_task_scope_return_error (scope, error);
}

AmlDownloadService *
aml_download_service_new (void)
{
  if (service == NULL)
    {
      service = g_new0 (AmlDownloadService, 1);
      g_queue_init (&service->interactive_waiting);
      g_queue_init (&service->bulk_waiting);
    }
  return service;
}

void
aml_download_service_free (AmlDownloadService *owned_service)
{
  if (owned_service != service)
    return;
  aml_download_shutdown ();
  if (service != NULL && service->interactive_running == 0 &&
      service->bulk_running == 0 &&
      g_queue_is_empty (&service->interactive_waiting) &&
      g_queue_is_empty (&service->bulk_waiting))
    g_clear_pointer (&service, g_free);
}

static AmlDownloadService *
get_service (void)
{
  return aml_download_service_new ();
}

static void
attach_task_scope (GTask *task)
{
  aml_task_scope_attach_task (task);
}

void
aml_download_shutdown (void)
{
  if (service == NULL)
    return;
  /* Requests own all callback state. Do not tear down the session while one
   * of them is still running or queued. The normal application shutdown
   * path calls this after its windows and tasks have gone away. */
  if (service->interactive_running != 0 || service->bulk_running != 0 ||
      !g_queue_is_empty (&service->interactive_waiting) ||
      !g_queue_is_empty (&service->bulk_waiting))
    return;

  g_clear_object (&service->session);
  g_clear_object (&service->cache_settings);
}

static SoupSession *
get_session (void)
{
  get_service ();
  if (service->session == NULL)
    /* max-conns is a construct property: it can only be given while
       the session is created, and it matches the cap on transfers so
       that our counter, not the socket pool, is what limits the
       downloads. */
    service->session = g_object_new (SOUP_TYPE_SESSION, "user-agent",
                            "AML (Another Minecraft Launcher)", "timeout",
                            (guint) 60, "max-conns", (gint) MAX_CONCURRENT,
                            "max-conns-per-host", (gint) MAX_CONCURRENT, NULL);

  return service->session;
}

/* ---------------------------------------------------------- *
 * One request
 * ---------------------------------------------------------- */

typedef struct _DownloadRequest
{
  GTask *task; /* the only reference: dropped when the request ends */

  /* Set once when the request is created. */
  char *url;
  char *dest_path;       /* NULL for a JSON request */
  char *expected_hash;   /* NULL: nothing to verify */
  GChecksumType hash_type;
  char *body;            /* NULL: a GET request */
  char *user_agent;      /* NULL: the session's own */
  AmlDownloadProgressFunc progress;
  gpointer user_data;
  gboolean json;
  gboolean api_cache;
  char *cache_path;
  char *global_cache_path;
  char *stale_json;
  char *cache_etag;
  char *response_etag;
  AmlSettingsCacheExpiration cache_expiration;
  JsonParser *pending_parser;
  char *pending_cache_data;

  /* What runs once this request holds a slot: the cache check and then
     the transfer for a file, the transfer alone for JSON. Holding the
     slot from the very start is what keeps a whole version's assets from
     opening thousands of files at once to hash them. */
  void (*begin) (struct _DownloadRequest *request);

  /* One attempt at a time. */
  SoupMessage *message;
  GInputStream *source;  /* response body, closed when it ends */
  GOutputStream *sink;   /* the temporary file */
  GBytes *chunk;         /* block being written */
  GChecksum *checksum;   /* hash of the received bytes */
  char *tmp_path;
  char *tmp_name;
  int parent_fd;
  int fd;
  goffset total;         /* announced length, -1 when unknown */
  goffset done;
  GError *pending_error; /* waits for the response stream to close */
  guint http_status;     /* status of a failed attempt */
  guint attempts;        /* attempts used, MAX_ATTEMPTS at most */
  gboolean hash_retried; /* a wrong hash is retried only once */
  gboolean holds_slot;
  guint priority;
  guint retry_source;    /* pending retry timeout, 0 while there is none */
  guint retry_cancel_source; /* cancellation watch for a pending retry */
  guint queue_cancel_source;
  AmlTaskScope *scope;
} DownloadRequest;

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
static gboolean queued_cancelled (gpointer user_data);
static void cache_open_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void hash_read_ready (GObject *source, GAsyncResult *result,
                             gpointer user_data);
static void send_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data);
static void json_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data);
static void cache_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void json_cache_start (DownloadRequest *request);
static void cache_load_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void request_succeed_json (DownloadRequest *request, JsonParser *parser);
static void global_cache_hash_ready (GObject *source, GAsyncResult *result,
                                     gpointer user_data);
static void global_cache_copy_ready (GObject *source, GAsyncResult *result,
                                     gpointer user_data);
static void global_cache_directory_ready (GObject *source,
                                          GAsyncResult *result,
                                          gpointer user_data);
static void global_cache_files_ready (GObject *source, GAsyncResult *result,
                                      gpointer user_data);
static GError *cancelled_error (void);
static gboolean request_was_cancelled (DownloadRequest *request, GError *error);
static void request_fail (DownloadRequest *request, GError *error);
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

static AmlSettings *
get_cache_settings (void)
{
  if (service->cache_settings == NULL)
    service->cache_settings = aml_settings_new ();

  return service->cache_settings;
}

static char *
api_cache_path (DownloadRequest *request)
{
  g_autofree char *directory = aml_cache_get_api_directory ();
  g_autofree char *key = g_strdup_printf ("%s\n%s\n%s",
                                           request->body != NULL ? "POST" : "GET",
                                           request->url,
                                           request->body != NULL ? request->body : "");
  g_autofree char *digest =
    g_compute_checksum_for_string (G_CHECKSUM_SHA256, key, -1);

  return g_build_filename (directory, digest, NULL);
}

static char *
global_cache_path (const char *hash)
{
  g_autofree char *directory = aml_cache_get_files_directory ();

  return g_build_filename (directory, hash, NULL);
}

static JsonParser *
cache_parser (const char *contents, gsize length, char **response,
              char **etag, gint64 *created)
{
  g_autoptr (JsonParser) envelope = json_parser_new ();
  JsonNode *root = NULL;
  JsonNode *node = NULL;
  const char *body = NULL;
  g_autoptr (JsonParser) parser = NULL;

  if (!json_parser_load_from_data (envelope, contents, length, NULL))
    return NULL;

  root = json_parser_get_root (envelope);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;

  node = json_object_get_member (json_node_get_object (root), "created");
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_INT64)
    return NULL;
  *created = json_node_get_int (node);

  node = json_object_get_member (json_node_get_object (root), "response");
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  body = json_node_get_string (node);

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, -1, NULL) ||
      json_parser_get_root (parser) == NULL)
    return NULL;

  if (response != NULL)
    *response = g_strdup (body);

  node = json_object_get_member (json_node_get_object (root), "etag");
  if (etag != NULL && node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    *etag = g_strdup (json_node_get_string (node));

  return g_steal_pointer (&parser);
}

static void
cache_write_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  aml_fs_replace_contents_finish (G_FILE (source), result, NULL, &error);
  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      request_fail (request, cancelled_error ());
      return;
    }

  if (error != NULL)
    g_debug ("the Modrinth API cache was not written: %s", error->message);
  request_succeed_json (request, g_steal_pointer (&request->pending_parser));
}

static void
cache_write_directory_done (GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
  GFile *file = user_data;
  DownloadRequest *request = g_object_get_data (G_OBJECT (file),
                                                "aml-cache-request");
  g_autoptr (GError) error = NULL;

  if (!g_file_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          request_fail (request, cancelled_error ());
          g_object_unref (file);
          return;
        }
      g_debug ("the Modrinth API cache directory was not created: %s",
               error->message);
      request_succeed_json (request, g_steal_pointer (&request->pending_parser));
      g_object_unref (file);
      return;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      request_fail (request, cancelled_error ());
      g_object_unref (file);
      return;
    }

  aml_fs_replace_contents_async (file, request->pending_cache_data,
                                  strlen (request->pending_cache_data),
                                  NULL, FALSE,
                                  G_FILE_CREATE_REPLACE_DESTINATION,
                                  g_task_get_cancellable (request->task),
                                  cache_write_done, request);
  g_object_unref (file);
}

static void
cache_write_parent_done (GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
  GFile *file = user_data;
  DownloadRequest *request = g_object_get_data (G_OBJECT (file),
                                                "aml-cache-request");
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) directory = g_file_get_parent (file);

  if (!g_file_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          request_fail (request, cancelled_error ());
          g_object_unref (file);
          return;
        }
      g_debug ("the Modrinth API cache parent was not created: %s",
               error->message);
      request_succeed_json (request, g_steal_pointer (&request->pending_parser));
      g_object_unref (file);
      return;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      request_fail (request, cancelled_error ());
      g_object_unref (file);
      return;
    }

  g_file_make_directory_async (directory, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               cache_write_directory_done, file);
}

static void
cache_write_cache_done (GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  GFile *file = user_data;
  DownloadRequest *request = g_object_get_data (G_OBJECT (file),
                                                "aml-cache-request");
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) directory = g_file_get_parent (file);
  g_autoptr (GFile) parent = g_file_get_parent (directory);

  if (!g_file_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          request_fail (request, cancelled_error ());
          g_object_unref (file);
          return;
        }
      g_debug ("the Modrinth API cache root was not created: %s",
               error->message);
      request_succeed_json (request, g_steal_pointer (&request->pending_parser));
      g_object_unref (file);
      return;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      request_fail (request, cancelled_error ());
      g_object_unref (file);
      return;
    }

  g_file_make_directory_async (parent, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               cache_write_parent_done, file);
}

static void
cache_write (DownloadRequest *request, const char *response)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *data = NULL;
  g_autoptr (GFile) file = g_file_new_for_path (request->cache_path);
  g_autoptr (GFile) directory = g_file_get_parent (file);
  g_autoptr (GFile) parent = g_file_get_parent (directory);
  g_autoptr (GFile) cache_root = g_file_get_parent (parent);
  gsize length = 0;

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "created");
  json_builder_add_int_value (builder, g_get_real_time () / G_USEC_PER_SEC);
  json_builder_set_member_name (builder, "response");
  json_builder_add_string_value (builder, response);
  if (request->response_etag != NULL)
    {
      json_builder_set_member_name (builder, "etag");
      json_builder_add_string_value (builder, request->response_etag);
    }
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  data = json_generator_to_data (generator, &length);

  /* The cache directory is normally already present. GIO's async mkdir
   * also handles its first creation; the cache write itself is async. */
  request->pending_cache_data = g_steal_pointer (&data);
  g_debug ("writing Modrinth API cache entry (%zu bytes)",
           strlen (request->pending_cache_data));
  g_object_set_data (G_OBJECT (file), "aml-cache-request", request);
  g_file_make_directory_async (cache_root, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               cache_write_cache_done,
                               g_object_ref (file));
}

static GError *
cancelled_error (void)
{
  return g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_CANCELLED,
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

static gint
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
 * Slots: at most MAX_CONCURRENT transfers at a time
 * ---------------------------------------------------------- */

static void
start_transfer (DownloadRequest *request)
{
  get_service ();
  gboolean interactive = request->priority >= DOWNLOAD_PRIORITY_INTERACTIVE;
  guint total = service->interactive_running + service->bulk_running;
  gboolean has_slot = total < MAX_CONCURRENT &&
                      (interactive || service->bulk_running < MAX_BULK_CONCURRENT);

  if (has_slot)
    {
      if (interactive)
        service->interactive_running++;
      else
        service->bulk_running++;
      request->holds_slot = TRUE;
      request->begin (request);
      return;
    }

  GQueue *queue = interactive ? &service->interactive_waiting
                              : &service->bulk_waiting;
  g_queue_insert_sorted (queue, request, compare_waiting_requests, NULL);

  GSource *source = g_cancellable_source_new (
    g_task_get_cancellable (request->task));
  g_source_set_callback (source, queued_cancelled, request, NULL);
  request->queue_cancel_source = g_source_attach (source, NULL);
  g_source_unref (source);
}

static void
release_slot (DownloadRequest *request)
{
  if (!request->holds_slot)
    return;

  request->holds_slot = FALSE;
  if (request->priority >= DOWNLOAD_PRIORITY_INTERACTIVE)
    service->interactive_running--;
  else
    service->bulk_running--;

  while (service->interactive_running + service->bulk_running < MAX_CONCURRENT)
    {
      DownloadRequest *next = NULL;

      if (!g_queue_is_empty (&service->interactive_waiting))
        next = g_queue_pop_head (&service->interactive_waiting);
      else if (service->bulk_running < MAX_BULK_CONCURRENT &&
               !g_queue_is_empty (&service->bulk_waiting))
        next = g_queue_pop_head (&service->bulk_waiting);

      if (next == NULL)
        break;

      if (next->queue_cancel_source != 0)
        {
          g_source_remove (next->queue_cancel_source);
          next->queue_cancel_source = 0;
        }

      if (next->priority >= DOWNLOAD_PRIORITY_INTERACTIVE)
        service->interactive_running++;
      else
        service->bulk_running++;
      next->holds_slot = TRUE;
      next->begin (next);
    }
}

/* ---------------------------------------------------------- *
 * The temporary file
 * ---------------------------------------------------------- */

/* Closing a local descriptor is a single system call and cannot wait
 * for data, so it is done synchronously here. */
static void
close_sink (DownloadRequest *request)
{
  if (request->sink != NULL)
    {
      g_output_stream_close (request->sink, NULL, NULL);
      g_clear_object (&request->sink);
    }

  request->fd = -1;
}

/* Removes what an attempt has written so far. */
static void
discard_temporary (DownloadRequest *request)
{
  if (request->tmp_path != NULL)
    {
      if (request->parent_fd >= 0 && request->tmp_name != NULL)
        unlinkat (request->parent_fd, request->tmp_name, 0);
      else
        aml_fs_unlink (request->tmp_path, NULL);
      g_clear_pointer (&request->tmp_path, g_free);
    }
  g_clear_pointer (&request->tmp_name, g_free);
  if (request->parent_fd >= 0)
    {
      close (request->parent_fd);
      request->parent_fd = -1;
    }

  close_sink (request);
}

static gboolean
open_temporary (DownloadRequest *request, GError **error)
{
  g_autofree char *directory = g_path_get_dirname (request->dest_path);
  int fd = -1;

  /* Do not create a temporary beside a destination reached through a
   * symlink. The download is later published with rename(), so accepting a
   * substituted parent would write into a directory other than the one the
   * caller selected. The instance installer creates real private temporary
   * directories, and rejecting symlink parents is the safe default for all
   * other callers too. */
  request->parent_fd = aml_fs_open_directory_nofollow (directory, error);
  if (request->parent_fd < 0)
    return FALSE;

  for (guint attempt = 0; attempt < 100; attempt++)
    {
      g_autofree char *name = g_strdup_printf (".aml-%08x%08x",
                                               g_random_int (),
                                               g_random_int ());
      fd = openat (request->parent_fd, name,
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (fd >= 0)
        {
          request->tmp_name = g_steal_pointer (&name);
          break;
        }
      if (errno != EEXIST)
        break;
    }

  if (fd < 0)
    {
      int errsv = errno;

      g_set_error (error, AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_IO,
                   "cannot create a temporary file in \"%s\": %s",
                   directory, g_strerror (errsv));
      close (request->parent_fd);
      request->parent_fd = -1;
      return FALSE;
    }

  request->tmp_path = g_build_filename (directory, request->tmp_name, NULL);
  request->fd = fd;
  request->sink = g_unix_output_stream_new (fd, TRUE);

  return TRUE;
}

/* ---------------------------------------------------------- *
 * Ending a request
 * ---------------------------------------------------------- */

static void
request_free (DownloadRequest *request)
{
  if (request->retry_source != 0)
    aml_task_scope_remove_source (request->scope, request->retry_source);
  if (request->retry_cancel_source != 0)
    g_source_remove (request->retry_cancel_source);
  if (request->queue_cancel_source != 0)
    g_source_remove (request->queue_cancel_source);

  close_sink (request);
  g_clear_pointer (&request->chunk, g_bytes_unref);
  g_clear_pointer (&request->checksum, g_checksum_free);
  g_clear_pointer (&request->tmp_path, g_free);
  g_clear_pointer (&request->tmp_name, g_free);
  aml_task_scope_unref (request->scope);
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

static void
request_fail (DownloadRequest *request, GError *error)
{
  discard_temporary (request);
  release_slot (request);
  aml_task_scope_return_error (request->scope, error);
  request_free (request);
}

static void
request_succeed (DownloadRequest *request)
{
  release_slot (request);
  aml_task_scope_return_boolean (request->scope, TRUE);
  request_free (request);
}

static void
request_succeed_json (DownloadRequest *request, JsonParser *parser)
{
  release_slot (request);
  aml_task_scope_return_pointer (request->scope, parser, g_object_unref);
  request_free (request);
}

/* ---------------------------------------------------------- *
 * Hashing a file already on disk
 *
 * GIO has no checksum helper, so the file is read asynchronously in
 * chunks and fed to a GChecksum. The caller gets the hex digest, or
 * the error of the read; G_IO_ERROR_NOT_FOUND simply means that
 * nothing is cached yet.
 * ---------------------------------------------------------- */

typedef struct
{
  GTask *task;
  AmlTaskScope *scope;
  GInputStream *stream;
  GChecksum *checksum;
} HashFile;

static void hash_read_next (HashFile *hash);

static void
hash_file_close (HashFile *hash)
{
  if (hash->stream != NULL)
    {
      g_input_stream_close (hash->stream, NULL, NULL);
      g_clear_object (&hash->stream);
    }
}

static void
hash_file_free (HashFile *hash)
{
  hash_file_close (hash);
  g_clear_pointer (&hash->checksum, g_checksum_free);
  aml_task_scope_unref (hash->scope);
  g_clear_object (&hash->task);
  g_free (hash);
}

static void
hash_file_async (GFile *file, GChecksumType type, GCancellable *cancellable,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  HashFile *hash = g_new0 (HashFile, 1);

  hash->task = g_task_new (file, cancellable, callback, user_data);
  aml_task_scope_attach_task (hash->task);
  hash->scope = aml_task_scope_from_task (hash->task);
  hash->checksum = g_checksum_new (type);

  g_file_read_async (file, G_PRIORITY_DEFAULT, cancellable, cache_open_ready,
                     hash);
}

static void
cache_open_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  HashFile *hash = user_data;
  g_autoptr (GError) error = NULL;

  hash->stream = G_INPUT_STREAM (
    g_file_read_finish (G_FILE (source), result, &error));

  if (hash->stream == NULL)
    {
      aml_task_scope_return_error (hash->scope, g_steal_pointer (&error));
      hash_file_free (hash);
      return;
    }

  hash_read_next (hash);
}

static void
hash_read_next (HashFile *hash)
{
  g_input_stream_read_bytes_async (hash->stream, CHUNK_SIZE,
                                   G_PRIORITY_DEFAULT,
                                    aml_task_scope_get_cancellable (hash->scope),
                                   hash_read_ready, hash);
}

static void
hash_read_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  HashFile *hash = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (bytes == NULL)
    {
      aml_task_scope_return_error (hash->scope, g_steal_pointer (&error));
      hash_file_free (hash);
      return;
    }

  gsize size = 0;
  const char *data = g_bytes_get_data (bytes, &size);

  if (size == 0)
    {
      aml_task_scope_return_pointer (
        hash->scope, g_strdup (g_checksum_get_string (hash->checksum)), g_free);
      hash_file_free (hash);
      return;
    }

  g_checksum_update (hash->checksum, (const guchar *) data, size);
  hash_read_next (hash);
}

static char *
hash_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
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

  hash_file_async (destination, request->hash_type,
                   g_task_get_cancellable (request->task), cache_ready,
                   request);
}

static void
global_cache_copy (DownloadRequest *request)
{
  g_autoptr (GFile) source = g_file_new_for_path (request->dest_path);
  g_autoptr (GFile) destination =
    g_file_new_for_path (request->global_cache_path);
  aml_fs_copy_async (source, destination, G_FILE_COPY_OVERWRITE,
                     G_PRIORITY_DEFAULT, g_task_get_cancellable (request->task),
                     NULL, NULL, global_cache_copy_ready, request);
}

static void
global_cache_directory_ready (GObject *source, GAsyncResult *result,
                               gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = aml_cache_get_files_directory ();
  g_autoptr (GFile) files = g_file_new_for_path (directory);

  if (!g_file_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          request_fail (request, cancelled_error ());
          return;
        }
      g_debug ("the global file cache directory was not created: %s",
               error->message);
      request_succeed (request);
      return;
    }

  g_file_make_directory_async (files, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               global_cache_files_ready, request);
}

static void
global_cache_files_ready (GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          request_fail (request, cancelled_error ());
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

  if (!aml_fs_copy_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("the global file cache was not written: %s", error->message);

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      request_fail (request, cancelled_error ());
      return;
    }

  request_succeed (request);
}

static void
global_cache_hash_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                         gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = aml_download_file_hash_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      request_fail (request, cancelled_error ());
      return;
    }

  if (digest != NULL &&
      g_ascii_strcasecmp (digest, request->expected_hash) == 0)
    {
      g_autoptr (GFile) source =
        g_file_new_for_path (request->global_cache_path);
      g_autoptr (GFile) destination = g_file_new_for_path (request->dest_path);

      aml_fs_copy_async (source, destination, G_FILE_COPY_OVERWRITE,
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
  AmlSettings *settings = get_cache_settings ();

  request->api_cache = aml_settings_get_cache_modrinth_api_responses (settings);
  request->cache_expiration = aml_settings_get_cache_expiration (settings);

  if (!request->api_cache)
    {
      start_attempt (request);
      return;
    }

  request->cache_path = api_cache_path (request);
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
          request_fail (request, cancelled_error ());
          return;
        }

      start_attempt (request);
      return;
    }

    {
      g_autofree char *etag = NULL;

      parser = cache_parser (contents, length, &response, &etag, &created);
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
          request_fail (request, cancelled_error ());
          return;
        }

      request_succeed_json (request, g_steal_pointer (&parser));
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
  g_autofree char *digest = hash_file_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      request_fail (request, cancelled_error ());
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
  request->global_cache_path = global_cache_path (request->expected_hash);
  {
    g_autoptr (GFile) global =
      g_file_new_for_path (request->global_cache_path);
    hash_file_async (global, request->hash_type,
                     g_task_get_cancellable (request->task),
                     global_cache_hash_ready, request);
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
static GError *
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
    return g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_RATE_LIMITED,
                        "HTTP 429 too many requests for %s", url);

  reason = soup_message_get_reason_phrase (request->message);

  return g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_HTTP, "HTTP %u %s for %s",
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

static void read_next_chunk (DownloadRequest *request);
static void end_attempt (DownloadRequest *request, GError *error);

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

static void
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
      end_attempt (request, NULL);
      return;
    }

  gsize size = 0;
  const char *data = NULL;

  request->chunk = g_steal_pointer (&bytes);
  data = g_bytes_get_data (request->chunk, &size);

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
          GError *io = g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_IO,
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
      request_fail (request, cancelled_error ());
      return;
    }

  if (request->expected_hash != NULL &&
      g_ascii_strcasecmp (g_checksum_get_string (request->checksum),
                          request->expected_hash) != 0)
    {
      g_autofree char *got =
        g_strdup (g_checksum_get_string (request->checksum));

      attempt_failed (request,
                      g_error_new (AML_DOWNLOAD_ERROR,
                                   AML_DOWNLOAD_ERROR_CHECKSUM,
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
                      g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_IO,
                                   "cannot flush \"%s\": %s",
                                   request->tmp_path, g_strerror (errsv)));
      return;
    }

  close_sink (request);

  g_autofree char *destination_name = g_path_get_basename (request->dest_path);
  if (renameat (request->parent_fd, request->tmp_name,
                request->parent_fd, destination_name) != 0)
    {
      int errsv = errno;

      attempt_failed (request,
                      g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_IO,
                                   "cannot rename \"%s\" to \"%s\": %s",
                                   request->tmp_path, request->dest_path,
                                   g_strerror (errsv)));
      return;
    }

  g_clear_pointer (&request->tmp_path, g_free);
  if (request->expected_hash != NULL)
    {
      g_autofree char *directory = aml_cache_get_files_directory ();
      g_autoptr (GFile) directory_file = g_file_new_for_path (directory);
      g_autoptr (GFile) parent = g_file_get_parent (directory_file);

      if (request->global_cache_path == NULL)
        request->global_cache_path = global_cache_path (request->expected_hash);
      g_file_make_directory_async (parent, G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (request->task),
                                   global_cache_directory_ready, request);
      return;
    }
  request_succeed (request);
}

/* Ends one attempt. A NULL @error means that a file body arrived
 * complete, which only a file request reports. The response stream is
 * always closed first so the connection goes back to the session. */
static void
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
      request_fail (request, cancelled_error ());
      return;
    }

  if (error->domain != AML_DOWNLOAD_ERROR)
    {
      /* The transport reports in its own domains: refused connection,
         name resolution, timeout, broken pipe. The caller sees one. */
      GError *wrapped =
        g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_NETWORK, "%s",
                     error->message);

      g_error_free (error);
      error = wrapped;
    }

  if (request->stale_json != NULL &&
      g_error_matches (error, AML_DOWNLOAD_ERROR,
                       AML_DOWNLOAD_ERROR_NETWORK))
    {
      g_autoptr (JsonParser) parser = json_parser_new ();

      if (json_parser_load_from_data (parser, request->stale_json, -1, NULL) &&
          json_parser_get_root (parser) != NULL)
        {
          g_clear_error (&error);
          request_succeed_json (request, g_steal_pointer (&parser));
          return;
        }
    }

  if (g_error_matches (error, AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_IO))
    retry = FALSE;
  else if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                            AML_DOWNLOAD_ERROR_CHECKSUM))
    {
      /* Wrong bytes: one fresh transfer, and no more than that. */
      retry = !request->hash_retried && request->attempts < MAX_ATTEMPTS;
      request->hash_retried = retry;
    }
  else if (g_error_matches (error, AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_HTTP))
    retry = request->http_status >= 500 && request->attempts < MAX_ATTEMPTS;
  else if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                            AML_DOWNLOAD_ERROR_RATE_LIMITED))
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
      discard_temporary (request);
      g_clear_error (&error);
      request->retry_source = g_timeout_add (delay_ms, retry_when_due, request);
      aml_task_scope_track_source (request->scope, request->retry_source);
      {
        GSource *source = g_cancellable_source_new (
          g_task_get_cancellable (request->task));

        g_source_set_callback (source, retry_cancelled, request, NULL);
        request->retry_cancel_source = g_source_attach (source, NULL);
        aml_task_scope_track_source (request->scope,
                                     request->retry_cancel_source);
        g_source_unref (source);
      }
      return;
    }

  request_fail (request, error);
}

static gboolean
retry_when_due (gpointer user_data)
{
  DownloadRequest *request = user_data;

  aml_task_scope_forget_source (request->scope, request->retry_source);
  request->retry_source = 0;
  if (request->retry_cancel_source != 0)
    {
      aml_task_scope_remove_source (request->scope,
                                    request->retry_cancel_source);
      request->retry_cancel_source = 0;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      request_fail (request, cancelled_error ());
      return G_SOURCE_REMOVE;
    }

  start_attempt (request);

  return G_SOURCE_REMOVE;
}

static gboolean
retry_cancelled (gpointer user_data)
{
  DownloadRequest *request = user_data;

  request->retry_cancel_source = 0;
  request_fail (request, cancelled_error ());
  return G_SOURCE_REMOVE;
}

static gboolean
queued_cancelled (gpointer user_data)
{
  DownloadRequest *request = user_data;
  GQueue *queue = request->priority >= DOWNLOAD_PRIORITY_INTERACTIVE
                    ? &service->interactive_waiting : &service->bulk_waiting;

  request->queue_cancel_source = 0;
  if (g_queue_remove (queue, request))
    request_fail (request, cancelled_error ());
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
      request_fail (request, cancelled_error ());
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

      request_fail (request,
                    g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_NETWORK,
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
      soup_session_send_and_read_async (get_session (), request->message,
                                        G_PRIORITY_DEFAULT, cancellable,
                                        json_ready, request);
      return;
    }

  if (!open_temporary (request, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      return;
    }

  if (request->expected_hash != NULL)
    request->checksum = g_checksum_new (request->hash_type);

  soup_session_send_async (get_session (), request->message,
                           G_PRIORITY_DEFAULT, cancellable, send_ready,
                           request);
}

/* ---------------------------------------------------------- *
 * JSON bodies
 * ---------------------------------------------------------- */

static void
json_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  GError *status_error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) parse_error = NULL;
  g_autofree char *response = NULL;
  gsize size = 0;
  const char *data = NULL;

  if (body == NULL)
    {
      end_attempt (request, g_steal_pointer (&error));
      return;
    }

  status_error = check_status (request);

  if (request->stale_json != NULL &&
      soup_message_get_status (request->message) == SOUP_STATUS_NOT_MODIFIED)
    {
      g_autoptr (JsonParser) stale = json_parser_new ();

      if (json_parser_load_from_data (stale, request->stale_json, -1, NULL) &&
          json_parser_get_root (stale) != NULL)
        {
          g_clear_error (&status_error);
          request_succeed_json (request, g_steal_pointer (&stale));
          return;
        }
    }

  if (status_error != NULL)
    {
      end_attempt (request, status_error);
      return;
    }

  parser = json_parser_new ();
  data = g_bytes_get_data (body, &size);

  {
    const char *etag = soup_message_headers_get_one (
      soup_message_get_response_headers (request->message), "ETag");

    g_clear_pointer (&request->response_etag, g_free);
    request->response_etag = g_strdup (etag);
  }

  if (size > MAX_JSON_RESPONSE_SIZE)
    {
      request_fail (request,
                    g_error_new (AML_DOWNLOAD_ERROR, AML_DOWNLOAD_ERROR_IO,
                                 "the JSON response is too large (%zu bytes)",
                                 size));
      return;
    }

  if (!json_parser_load_from_data (parser, data != NULL ? data : "",
                                   (gssize) size, &parse_error))
    {
      /* A body that is not JSON would come back unchanged on a retry,
         so the parse error is reported as it is. */
      request_fail (request, g_steal_pointer (&parse_error));
      return;
    }

  if (json_parser_get_root (parser) == NULL)
    {
      /* json-glib accepts an empty document without complaint; there
         is nothing to hand to a caller though. */
      request_fail (request,
                    g_error_new_literal (JSON_PARSER_ERROR,
                                         JSON_PARSER_ERROR_PARSE,
                                         "the response body holds no JSON"));
      return;
    }

  if (request->api_cache && request->cache_path != NULL)
    {
      data = g_bytes_get_data (body, &size);
      response = g_strndup (data, size);
      request->pending_parser = g_steal_pointer (&parser);
      cache_write (request, response);
      return;
    }

  request_succeed_json (request, g_steal_pointer (&parser));
}

/* ---------------------------------------------------------- *
 * The public interface
 * ---------------------------------------------------------- */

void
aml_download_file_async (const char *url,
                         const char *dest_path,
                         const char *expected_hash,
                         GChecksumType hash_type,
                         GCancellable *cancellable,
                         AmlDownloadProgressFunc progress,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
  g_return_if_fail (url != NULL);
  g_return_if_fail (dest_path != NULL);
  g_return_if_fail (g_path_is_absolute (dest_path));

  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  attach_task_scope (task);

  /* Cancellation is reported by this module, in its own error domain,
     so GTask must not replace the error with G_IO_ERROR_CANCELLED. */
  g_task_set_check_cancellable (task, FALSE);

  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);

      download_return_new_error (task, AML_DOWNLOAD_ERROR,
                               AML_DOWNLOAD_ERROR_NETWORK,
                               "\"%s\" is not a usable download url", shown);
      return;
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;

  request->task = g_steal_pointer (&task);
  request->scope = aml_task_scope_ref (
    g_object_get_data (G_OBJECT (request->task), "aml-task-scope"));
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

  start_transfer (request);
}

gboolean
aml_download_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
aml_download_json_async (const char *url,
                         GCancellable *cancellable,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
  g_return_if_fail (url != NULL);

  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  attach_task_scope (task);

  /* See aml_download_file_async(). */
  g_task_set_check_cancellable (task, FALSE);

  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);

      download_return_new_error (task, AML_DOWNLOAD_ERROR,
                               AML_DOWNLOAD_ERROR_NETWORK,
                               "\"%s\" is not a usable download url", shown);
      return;
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;

  request->task = g_steal_pointer (&task);
  request->scope = aml_task_scope_ref (
    g_object_get_data (G_OBJECT (request->task), "aml-task-scope"));
  request->url = g_strdup (url);
  request->json = TRUE;
  request->user_data = user_data;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_INTERACTIVE;
  request->begin = start_attempt;

  start_transfer (request);
}

void
aml_download_set_cache_settings (AmlSettings *settings)
{
  g_return_if_fail (settings == NULL || AML_IS_SETTINGS (settings));

  get_service ();
  g_set_object (&service->cache_settings, settings);
}

void
aml_download_file_hash_async (GFile *file, GChecksumType type,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
  g_return_if_fail (G_IS_FILE (file));

  hash_file_async (file, type, cancellable, callback, user_data);
}

char *
aml_download_file_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return hash_file_finish (result, error);
}

JsonParser *
aml_download_json_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

void
aml_download_json_post_async (const char           *url,
                              const char           *body,
                              const char           *user_agent,
                              GCancellable         *cancellable,
                              GAsyncReadyCallback   callback,
                              gpointer              user_data)
{
  g_return_if_fail (url != NULL);
  g_return_if_fail (body != NULL);

  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  attach_task_scope (task);

  /* See aml_download_file_async(). */
  g_task_set_check_cancellable (task, FALSE);

  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);

      download_return_new_error (task, AML_DOWNLOAD_ERROR,
                               AML_DOWNLOAD_ERROR_NETWORK,
                               "\"%s\" is not a usable request url", shown);
      return;
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;

  request->task = g_steal_pointer (&task);
  request->scope = aml_task_scope_ref (
    g_object_get_data (G_OBJECT (request->task), "aml-task-scope"));
  request->url = g_strdup (url);
  request->body = g_strdup (body);
  request->user_agent = g_strdup (user_agent);
  request->json = TRUE;
  request->user_data = user_data;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_INTERACTIVE;
  request->begin = start_attempt;

  start_transfer (request);
}

void
aml_download_json_cached_async (const char *url, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail (url != NULL);
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  attach_task_scope (task);

  g_task_set_check_cancellable (task, FALSE);
  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);
      download_return_new_error (task, AML_DOWNLOAD_ERROR,
                               AML_DOWNLOAD_ERROR_NETWORK,
                               "\"%s\" is not a usable download url", shown);
      return;
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;
  request->task = g_steal_pointer (&task);
  request->scope = aml_task_scope_ref (
    g_object_get_data (G_OBJECT (request->task), "aml-task-scope"));
  request->url = g_strdup (url);
  request->json = TRUE;
  request->api_cache = TRUE;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_INTERACTIVE;
  request->begin = json_cache_start;
  start_transfer (request);
}

void
aml_download_json_post_cached_async (const char *url, const char *body,
                                     const char *user_agent,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail (url != NULL);
  g_return_if_fail (body != NULL);
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  attach_task_scope (task);

  g_task_set_check_cancellable (task, FALSE);
  if (!usable_url (url))
    {
      g_autofree char *shown = url_for_message (url);
      download_return_new_error (task, AML_DOWNLOAD_ERROR,
                               AML_DOWNLOAD_ERROR_NETWORK,
                               "\"%s\" is not a usable request url", shown);
      return;
    }

   DownloadRequest *request = g_new0 (DownloadRequest, 1);
   request->parent_fd = -1;
  request->task = g_steal_pointer (&task);
  request->scope = aml_task_scope_ref (
    g_object_get_data (G_OBJECT (request->task), "aml-task-scope"));
  request->url = g_strdup (url);
  request->body = g_strdup (body);
  request->user_agent = g_strdup (user_agent);
  request->json = TRUE;
  request->api_cache = TRUE;
  request->fd = -1;
  request->total = -1;
  request->priority = DOWNLOAD_PRIORITY_INTERACTIVE;
  request->begin = json_cache_start;
  start_transfer (request);
}
