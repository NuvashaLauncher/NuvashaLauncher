/* File and API response cache flow for downloads. */
#include "nuvashalauncher-download-private.h"
#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-task-private.h"
#include <string.h>

static void cache_ready (GObject *, GAsyncResult *, gpointer);
static void cache_load_ready (GObject *, GAsyncResult *, gpointer);
static void global_cache_copy_ready (GObject *, GAsyncResult *, gpointer);
static void global_cache_copy_hash_ready (GObject *, GAsyncResult *, gpointer);
static void global_cache_files_ready (GObject *, GAsyncResult *, gpointer);
void global_cache_directory_ready (GObject *, GAsyncResult *, gpointer);

/* ---------------------------------------------------------- *
 * Checking the cache before downloading
 * ---------------------------------------------------------- */

void
nuvashalauncher_download_file_cache_start (DownloadRequest *request)
{
  if (request->expected_hash == NULL)
    {
      /* Without an expected hash there is nothing an existing file
         could be compared against. The slot is already held. */
      start_attempt (request);
      return;
    }

  g_autoptr (GFile) destination = g_file_new_for_path (request->dest_path);

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_hash_file_async (destination, request->hash_type, cache_ready,
                                  request),
    g_task_get_cancellable (request->task));
}

static void
global_cache_copy (DownloadRequest *request)
{
  g_autoptr (GFile) source = g_file_new_for_path (request->dest_path);
  g_autoptr (GFile) destination =
    g_file_new_for_path (request->global_cache_path);
  nuvashalauncher_fs_copy_async (source, destination, G_FILE_COPY_OVERWRITE,
                     G_PRIORITY_DEFAULT, g_task_get_cancellable (request->task),
                     NULL, NULL, global_cache_copy_ready, request);
}

void
global_cache_directory_ready (GObject *source, GAsyncResult *result,
                               gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = nuvashalauncher_cache_get_files_directory ();
  g_autoptr (GFile) files = g_file_new_for_path (directory);

  if (!nuvashalauncher_fs_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
          return;
        }
      g_debug ("the global file cache directory was not created: %s",
               error->message);
      request_succeed (request);
      return;
    }

  nuvashalauncher_fs_make_directory_async (files, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               global_cache_files_ready, request);
}

static void
global_cache_files_ready (GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_fs_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
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

  if (!nuvashalauncher_fs_copy_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("the global file cache was not written: %s", error->message);

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
      return;
    }

  /* The source was hashed before the asynchronous copy.  Hash the bytes
   * actually adopted at the destination as well, so a replacement during
   * the copy cannot satisfy the request. */
  {
    g_autoptr (GFile) destination = g_file_new_for_path (request->dest_path);
    nuvashalauncher_task_scope_release_when_done (
      nuvashalauncher_download_hash_file_async (destination, request->hash_type,
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
  g_autofree char *digest = nuvashalauncher_download_file_hash_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
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
  g_autofree char *digest = nuvashalauncher_download_file_hash_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
      return;
    }

  if (digest != NULL &&
      g_ascii_strcasecmp (digest, request->expected_hash) == 0)
    {
      g_autoptr (GFile) cache_file =
        g_file_new_for_path (request->global_cache_path);
      g_autoptr (GFile) destination = g_file_new_for_path (request->dest_path);

       nuvashalauncher_fs_copy_async (cache_file, destination, G_FILE_COPY_OVERWRITE,
                         G_PRIORITY_DEFAULT,
                         g_task_get_cancellable (request->task), NULL, NULL,
                         global_cache_copy_ready, request);
      return;
    }

  start_attempt (request);
}

void
json_cache_start (DownloadRequest *request)
{
  nuvashalauncherSettings *settings = nuvashalauncher_download_get_cache_settings (request);

  request->api_cache = nuvashalauncher_settings_get_cache_modrinth_api_responses (settings);
  request->cache_expiration = nuvashalauncher_settings_get_cache_expiration (settings);

  if (!request->api_cache)
    {
      start_attempt (request);
      return;
    }

  request->cache_path = nuvashalauncher_download_api_cache_path (request);
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
          nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
          return;
        }

      start_attempt (request);
      return;
    }

    {
      g_autofree char *etag = NULL;

      parser = nuvashalauncher_download_cache_parser (contents, length, &response, &etag, &created);
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
          nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
          return;
        }

      nuvashalauncher_download_request_succeed_json (request, g_steal_pointer (&parser));
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
    g_autofree char *digest = nuvashalauncher_download_hash_file_finish (result, &error);

  if (request_was_cancelled (request, error))
    {
      nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
      return;
    }

  if (digest != NULL &&
      g_ascii_strcasecmp (digest, request->expected_hash) == 0)
    {
      /* The file on disk already holds exactly what was asked for. */
      request_succeed (request);
      return;
    }

  g_clear_error (&error);
  request->global_cache_path = nuvashalauncher_download_global_cache_path (request->expected_hash);
  {
    g_autoptr (GFile) global =
      g_file_new_for_path (request->global_cache_path);
    nuvashalauncher_task_scope_release_when_done (
      nuvashalauncher_download_hash_file_async (global, request->hash_type,
                                    global_cache_hash_ready, request),
      g_task_get_cancellable (request->task));
  }
}
