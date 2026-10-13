/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* API/file cache keys and JSON envelope parsing for the download engine. */

#include "aml-download-private.h"
#include "aml-cache.h"
#include "aml-fs.h"

#include <json-glib/json-glib.h>

AmlSettings *
aml_download_get_cache_settings (DownloadRequest *request)
{
  AmlDownloadService *service = request->service;
  if (service->cache_settings == NULL)
    service->cache_settings = aml_settings_new ();

  return service->cache_settings;
}

char *
aml_download_api_cache_path (DownloadRequest *request)
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

char *
aml_download_global_cache_path (const char *hash)
{
  g_autofree char *directory = aml_cache_get_files_directory ();

  return g_build_filename (directory, hash, NULL);
}

JsonParser *
aml_download_cache_parser (const char *contents, gsize length, char **response,
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

void
aml_download_cache_write_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  aml_fs_replace_contents_finish (G_FILE (source), result, NULL, &error);
  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      aml_download_request_fail (request, aml_download_cancelled_error ());
      return;
    }

  if (error != NULL)
    g_debug ("the Modrinth API cache was not written: %s", error->message);
  aml_download_request_succeed_json (request, g_steal_pointer (&request->pending_parser));
}

void
aml_download_cache_write_directory_done (GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
  GFile *file = user_data;
  DownloadRequest *request = g_object_get_data (G_OBJECT (file),
                                                "aml-cache-request");
  g_autoptr (GError) error = NULL;

  if (!aml_fs_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          aml_download_request_fail (request, aml_download_cancelled_error ());
          g_object_unref (file);
          return;
        }
      g_debug ("the Modrinth API cache directory was not created: %s",
               error->message);
      aml_download_request_succeed_json (request, g_steal_pointer (&request->pending_parser));
      g_object_unref (file);
      return;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      aml_download_request_fail (request, aml_download_cancelled_error ());
      g_object_unref (file);
      return;
    }

  aml_fs_replace_contents_async (file, request->pending_cache_data,
                                  strlen (request->pending_cache_data),
                                  NULL, FALSE,
                                  G_FILE_CREATE_REPLACE_DESTINATION,
                                  g_task_get_cancellable (request->task),
                                  aml_download_cache_write_done, request);
  g_object_unref (file);
}

void
aml_download_cache_write_parent_done (GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
  GFile *file = user_data;
  DownloadRequest *request = g_object_get_data (G_OBJECT (file),
                                                "aml-cache-request");
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) directory = g_file_get_parent (file);

  if (!aml_fs_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          aml_download_request_fail (request, aml_download_cancelled_error ());
          g_object_unref (file);
          return;
        }
      g_debug ("the Modrinth API cache parent was not created: %s",
               error->message);
      aml_download_request_succeed_json (request, g_steal_pointer (&request->pending_parser));
      g_object_unref (file);
      return;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      aml_download_request_fail (request, aml_download_cancelled_error ());
      g_object_unref (file);
      return;
    }

  aml_fs_make_directory_async (directory, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               aml_download_cache_write_directory_done, file);
}

void
aml_download_cache_write_cache_done (GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  GFile *file = user_data;
  DownloadRequest *request = g_object_get_data (G_OBJECT (file),
                                                "aml-cache-request");
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) directory = g_file_get_parent (file);
  g_autoptr (GFile) parent = g_file_get_parent (directory);

  if (!aml_fs_make_directory_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
        {
          aml_download_request_fail (request, aml_download_cancelled_error ());
          g_object_unref (file);
          return;
        }
      g_debug ("the Modrinth API cache root was not created: %s",
               error->message);
      aml_download_request_succeed_json (request, g_steal_pointer (&request->pending_parser));
      g_object_unref (file);
      return;
    }

  if (g_cancellable_is_cancelled (g_task_get_cancellable (request->task)))
    {
      aml_download_request_fail (request, aml_download_cancelled_error ());
      g_object_unref (file);
      return;
    }

  aml_fs_make_directory_async (parent, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               aml_download_cache_write_parent_done, file);
}

void
aml_download_cache_write (DownloadRequest *request, const char *response)
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
  aml_fs_make_directory_async (cache_root, G_PRIORITY_DEFAULT,
                               g_task_get_cancellable (request->task),
                               aml_download_cache_write_cache_done,
                               g_object_ref (file));
}

