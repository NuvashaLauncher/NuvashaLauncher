/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "nuvashalauncher-minecraft-resolve-private.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-fs.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ASSET_BUCKET_LENGTH 2
#define ASSET_RESOURCE_PREFIX "minecraft/"
#define ASSET_SHA1_LENGTH 40

/* Asset-index planning, object downloads, and virtual asset copies. */

static char *
asset_object_path (const char *objects_dir, const char *hash, char **bucket)
{
  g_autofree char *name = NULL;
  g_autofree char *directory = NULL;

  if (objects_dir == NULL || hash == NULL || strlen (hash) != ASSET_SHA1_LENGTH)
    return NULL;

  for (const char *p = hash; *p != '\0'; p++)
    if (!g_ascii_isxdigit (*p))
      return NULL;

  name = g_strndup (hash, ASSET_BUCKET_LENGTH);
  directory = g_build_filename (objects_dir, name, NULL);

  if (bucket != NULL)
    *bucket = g_strdup (name);

  return g_build_filename (directory, hash, NULL);
}

typedef struct {
  char *source;
  char *target;
} VirtualCopyItem;

static void
virtual_copy_item_free (VirtualCopyItem *item)
{
  if (item == NULL)
    return;

  g_free (item->source);
  g_free (item->target);
  g_free (item);
}

typedef struct {
  ResolveRequest *request;
  GPtrArray *items;
  guint copied;
} VirtualCopiesOp;

static void
virtual_copies_op_free (VirtualCopiesOp *op)
{
  if (op == NULL)
    return;

  g_clear_pointer (&op->items, g_ptr_array_unref);
  g_free (op);
}

static gboolean
copy_file_with_ficlone (const char   *source,
                        const char   *target,
                        GCancellable *cancellable,
                        GError      **error)
{
  if (nuvashalauncher_fs_try_clone_path (source, target, TRUE))
    return TRUE;

  {
    g_autoptr (GFile) source_file = g_file_new_for_path (source);
    g_autoptr (GFile) target_file = g_file_new_for_path (target);

    return g_file_copy (source_file, target_file, G_FILE_COPY_OVERWRITE,
                        cancellable, NULL, NULL, error);
  }
}

static void
virtual_copies_thread (GTask        *task,
                       gpointer      source_object G_GNUC_UNUSED,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  VirtualCopiesOp *op = task_data;
  g_autoptr (GError) error = NULL;

  for (guint i = 0; i < op->items->len; i++)
    {
      const VirtualCopyItem *item = g_ptr_array_index (op->items, i);
      GStatBuf st;

      if (g_task_return_error_if_cancelled (task))
        return;

      if (g_lstat (item->target, &st) == 0)
        continue;

      if (!nuvashalauncher_resolve_ensure_parent_directory (item->target, &error) ||
          !copy_file_with_ficlone (item->source, item->target, cancellable, &error))
        {
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }

      op->copied++;
    }

  g_task_return_boolean (task, TRUE);
}

static void
virtual_copies_ready (GObject      *source G_GNUC_UNUSED,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  VirtualCopiesOp *op = user_data;
  ResolveRequest *request = op->request;
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      operation_finished (request, resolve_finish, g_steal_pointer (&error));
      return;
    }

  if (op->copied > 0)
    {
      request->total += op->copied;
      request->done += op->copied;
      report (request);
    }

  operation_finished (request, resolve_finish, NULL);
}

/* Copies the objects the index expects to find under the virtual
   directory of the assets. A copy that is already there is left alone,
   because a second resolve must not rewrite what the first one wrote. */
void
resolve_copies (ResolveRequest *request)
{
  VirtualCopiesOp *op = NULL;
  GHashTableIter iter;
  gpointer key, value;

  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  op = g_new0 (VirtualCopiesOp, 1);
  op->request = request;
  op->items = g_ptr_array_new_with_free_func ((GDestroyNotify) virtual_copy_item_free);

  g_hash_table_iter_init (&iter, request->asset_paths);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *path = key;
      const char *hash = value;
      g_autofree char *source = NULL;
      VirtualCopyItem *item = NULL;

      if (path == NULL || hash == NULL)
        continue;

      source = asset_object_path (request->objects_dir, hash, NULL);
      if (source == NULL)
        continue;

      item = g_new0 (VirtualCopyItem, 1);
      item->source = g_steal_pointer (&source);
      item->target = g_build_filename (request->virtual_dir,
                                       request->asset_index_name, path, NULL);
      g_ptr_array_add (op->items, item);
    }

  if (op->items->len == 0)
    {
      virtual_copies_op_free (op);
      report (request);
      resolve_finish (request);
      return;
    }

  operation_started (request);
  {
    GTask *worker = g_task_new (NULL, request->cancellable, virtual_copies_ready, op);
    g_task_set_task_data (worker, op, (GDestroyNotify) virtual_copies_op_free);
    g_task_run_in_thread (worker, virtual_copies_thread);
    g_object_unref (worker);
  }
}

/* Every object of the index is on the disk now. */
static void
assets_downloaded (ResolveRequest *request)
{
  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  /* A virtual index is read as files, so the game is pointed at the
     copies; every other index is read from the objects themselves. */
  if (request->asset_virtual)
    resolve_copies (request);
  else
    resolve_finish (request);
}

static void
objects_downloaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  PendingOperation *pending = user_data;
  ResolveRequest *request = pending->request;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_download_file_finish (result, &error))
    operation_finished (request, assets_downloaded, g_steal_pointer (&error));
  else
    {
      request->done++;
      report (request);
      operation_finished (request, assets_downloaded, NULL);
    }

  g_free (pending);
}

typedef struct
{
  ResolveRequest *request;
  char *path;
  char *objects_dir;
  GHashTable *verified_hashes;
  GHashTable *ensured_buckets;
} AssetIndexLoad;

static void
asset_index_load_free (AssetIndexLoad *load)
{
  if (load == NULL)
    return;

  g_free (load->path);
  g_free (load->objects_dir);
  g_clear_pointer (&load->verified_hashes, g_hash_table_unref);
  g_clear_pointer (&load->ensured_buckets, g_hash_table_unref);
  g_free (load);
}

static gboolean
file_matches_sha1 (const char *path, const char *expected_hash, gint64 expected_size, GChecksum *checksum)
{
  int fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat st;
  guint8 buf[65536];

  if (fd < 0)
    return FALSE;

  if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode))
    {
      close (fd);
      return FALSE;
    }

  /* Quick reject: if size is known and file size does not match, avoid reading */
  if (expected_size >= 0 && (gint64) st.st_size != expected_size)
    {
      close (fd);
      return FALSE;
    }

  g_checksum_reset (checksum);
  for (;;)
    {
      ssize_t n = read (fd, buf, sizeof buf);
      if (n > 0)
        g_checksum_update (checksum, buf, (gssize) n);
      else if (n == 0)
        break;
      else if (errno != EINTR)
        {
          close (fd);
          return FALSE;
        }
    }

  close (fd);
  return g_ascii_strcasecmp (g_checksum_get_string (checksum), expected_hash) == 0;
}

static void
asset_index_parse_thread (GTask *task,
                          gpointer source_object G_GNUC_UNUSED,
                          gpointer task_data,
                          GCancellable *cancellable G_GNUC_UNUSED)
{
  AssetIndexLoad *load = task_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autofree char *text = NULL;
  gsize length = 0;

  if (!nuvashalauncher_fs_get_contents_within (load->path, NUVASHALAUNCHER_FS_MAX_ASSET_INDEX_SIZE,
                                               &text, &length, &error) ||
      !json_parser_load_from_data (parser, text, (gssize) length, &error) ||
      !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      g_autoptr (GError) local = g_steal_pointer (&error);

      g_task_return_new_error (task,
                               NUVASHALAUNCHER_RESOLVE_ERROR,
                               NUVASHALAUNCHER_RESOLVE_ERROR_ASSETS,
                               "the asset index \"%s\" cannot be read: %s",
                               load->path,
                               local != NULL ? local->message
                                             : "it is not a JSON object");
      return;
    }

  {
    JsonObject *root = json_node_get_object (json_parser_get_root (parser));
    JsonNode *objects_node = json_object_has_member (root, "objects")
                               ? json_object_get_member (root, "objects")
                               : NULL;

    if (objects_node != NULL && JSON_NODE_HOLDS_OBJECT (objects_node) &&
        load->objects_dir != NULL)
      {
        JsonObject *objects = json_node_get_object (objects_node);
        JsonObjectIter iter;
        const char *name = NULL;
        JsonNode *node = NULL;
        g_autoptr (GHashTable) seen =
          g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA1);

        load->verified_hashes =
          g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        load->ensured_buckets =
          g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

        json_object_iter_init (&iter, objects);
        while (json_object_iter_next (&iter, &name, &node))
          {
            const char *hash = name;
            g_autofree char *bucket = NULL;
            g_autofree char *destination = NULL;

            gint64 expected_size = -1;

            if (g_task_return_error_if_cancelled (task))
              return;

            if (!JSON_NODE_HOLDS_OBJECT (node))
              continue;

            {
              JsonObject *object = json_node_get_object (node);
              if (json_object_has_member (object, "hash"))
                {
                  JsonNode *hash_node = json_object_get_member (object, "hash");
                  if (!JSON_NODE_HOLDS_VALUE (hash_node) ||
                      json_node_get_value_type (hash_node) != G_TYPE_STRING)
                    continue;
                  hash = json_node_get_string (hash_node);
                }
              if (json_object_has_member (object, "size"))
                {
                  JsonNode *size_node = json_object_get_member (object, "size");
                  if (JSON_NODE_HOLDS_VALUE (size_node) &&
                      json_node_get_value_type (size_node) == G_TYPE_INT64)
                    expected_size = json_node_get_int (size_node);
                }
            }

            if (hash == NULL || g_hash_table_contains (seen, hash))
              continue;
            g_hash_table_add (seen, g_strdup (hash));

            destination = asset_object_path (load->objects_dir, hash, &bucket);
            if (destination == NULL)
              continue;

            if (file_matches_sha1 (destination, hash, expected_size, checksum))
              g_hash_table_add (load->verified_hashes, g_strdup (hash));
            else if (bucket != NULL &&
                     !g_hash_table_contains (load->ensured_buckets, bucket) &&
                     nuvashalauncher_resolve_ensure_parent_directory (destination, NULL))
              g_hash_table_add (load->ensured_buckets, g_steal_pointer (&bucket));
          }
      }
  }

  g_task_return_pointer (task, g_steal_pointer (&parser), g_object_unref);
}

static void
asset_index_parsed_ready (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result,
                          gpointer user_data)
{
  AssetIndexLoad *load = user_data;
  ResolveRequest *request = load->request;
  const char *path = load->path;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = g_task_propagate_pointer (G_TASK (result), &error);
  JsonNode *root_node = NULL;
  JsonObject *root = NULL;
  JsonObject *objects = NULL;
  JsonObjectIter iter;
  const char *name = NULL;
  JsonNode *node = NULL;

  if (parser == NULL)
    {
      asset_index_load_free (load);
      operation_finished (request, assets_downloaded, g_steal_pointer (&error));
      return;
    }

  root_node = json_parser_get_root (parser);
  root = json_node_get_object (root_node);

  if (!json_object_has_member (root, "objects") ||
      !JSON_NODE_HOLDS_OBJECT (json_object_get_member (root, "objects")))
    {
      GError *missing_err = g_error_new (
        NUVASHALAUNCHER_RESOLVE_ERROR, NUVASHALAUNCHER_RESOLVE_ERROR_ASSETS,
        "the asset index \"%s\" has no objects", path);
      asset_index_load_free (load);
      operation_finished (request, assets_downloaded, missing_err);
      return;
    }

  objects = json_object_get_object_member (root, "objects");
  request->asset_virtual = json_object_has_member (root, "virtual") &&
                           json_object_get_boolean_member (root, "virtual");
  request->asset_map_to_resources =
    json_object_has_member (root, "map_to_resources") &&
    json_object_get_boolean_member (root, "map_to_resources");

  g_autoptr (GHashTable) seen_hashes =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) ensured_buckets =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  request->total = request->done + json_object_get_size (objects);
  report (request);

  json_object_iter_init (&iter, objects);

  while (json_object_iter_next (&iter, &name, &node))
    {
      const char *hash = name;
      const char *asset_path = NULL;
      g_autofree char *destination = NULL;
      g_autofree char *bucket = NULL;
      g_autofree char *url = NULL;
      g_autoptr (GError) local = NULL;
      PendingOperation *pending;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        {
          GError *bad_err = g_error_new (
            NUVASHALAUNCHER_RESOLVE_ERROR, NUVASHALAUNCHER_RESOLVE_ERROR_ASSETS,
            "the asset \"%s\" of \"%s\" is not an object", name, path);
          asset_index_load_free (load);
          request_fail (request, bad_err);
          g_cancellable_cancel (request->cancellable);
          operation_finished (request, assets_downloaded, NULL);
          return;
        }

      {
        JsonObject *object = json_node_get_object (node);

        if (json_object_has_member (object, "hash"))
          {
            JsonNode *hash_node = json_object_get_member (object, "hash");

            if (!JSON_NODE_HOLDS_VALUE (hash_node) ||
                json_node_get_value_type (hash_node) != G_TYPE_STRING)
              {
                GError *hash_err = g_error_new (
                  NUVASHALAUNCHER_RESOLVE_ERROR,
                  NUVASHALAUNCHER_RESOLVE_ERROR_ASSETS,
                  "the asset \"%s\" has an invalid hash", name);
                asset_index_load_free (load);
                request_fail (request, hash_err);
                g_cancellable_cancel (request->cancellable);
                operation_finished (request, assets_downloaded, NULL);
                return;
              }

            hash = json_node_get_string (hash_node);
          }

        if (json_object_has_member (object, "path"))
          {
            JsonNode *path_node = json_object_get_member (object, "path");

            if (JSON_NODE_HOLDS_VALUE (path_node) &&
                json_node_get_value_type (path_node) == G_TYPE_STRING)
              asset_path = json_node_get_string (path_node);
          }
        else if (request->asset_virtual)
          {
            asset_path = name;
          }
      }

      if (asset_path != NULL && !nuvashalauncher_resolve_entry_name_is_safe (asset_path))
        {
          GError *path_err = g_error_new (
            NUVASHALAUNCHER_RESOLVE_ERROR, NUVASHALAUNCHER_RESOLVE_ERROR_UNSAFE_PATH,
            "the asset \"%s\" of \"%s\" would be copied to "
            "\"%s\", outside the assets directory",
            hash, path, asset_path);
          asset_index_load_free (load);
          request_fail (request, path_err);
          g_cancellable_cancel (request->cancellable);
          operation_finished (request, assets_downloaded, NULL);
          return;
        }

      if (asset_path != NULL)
        {
          /* A map_to_resources index already carries the prefix the
             game looks under; one that does not is read as a resource
             of the game. */
          g_autofree char *target_rel = NULL;
          if (request->asset_virtual && request->asset_map_to_resources &&
              !g_str_has_prefix (asset_path, ASSET_RESOURCE_PREFIX))
            target_rel = g_strconcat (ASSET_RESOURCE_PREFIX, asset_path, NULL);
          else
            target_rel = g_strdup (asset_path);

          g_hash_table_insert (request->asset_paths, g_steal_pointer (&target_rel),
                               g_strdup (hash));
        }

      if (!g_hash_table_contains (seen_hashes, hash))
        {
          g_hash_table_add (seen_hashes, g_strdup (hash));
          g_ptr_array_add (request->asset_hashes, g_strdup (hash));

          destination = asset_object_path (request->objects_dir, hash, &bucket);

          if (destination == NULL)
            {
              GError *dest_err = g_error_new (
                NUVASHALAUNCHER_RESOLVE_ERROR,
                NUVASHALAUNCHER_RESOLVE_ERROR_ASSETS,
                "\"%s\" is not a usable asset name", hash);
              asset_index_load_free (load);
              request_fail (request, dest_err);
              g_cancellable_cancel (request->cancellable);
              operation_finished (request, assets_downloaded, NULL);
              return;
            }

          if (load->verified_hashes != NULL &&
              g_hash_table_contains (load->verified_hashes, hash))
            {
              request->done++;
              continue;
            }

          if ((load->ensured_buckets == NULL ||
               !g_hash_table_contains (load->ensured_buckets, bucket)) &&
              !g_hash_table_contains (ensured_buckets, bucket))
            {
              if (!nuvashalauncher_resolve_ensure_parent_directory (destination, &local))
                {
                  asset_index_load_free (load);
                  request_fail (request, g_steal_pointer (&local));
                  g_cancellable_cancel (request->cancellable);
                  operation_finished (request, assets_downloaded, NULL);
                  return;
                }
              g_hash_table_add (ensured_buckets, g_steal_pointer (&bucket));
              bucket = g_strndup (hash, 2);
            }

          url = g_strdup_printf ("%s/%s/%s", request->asset_base_url, bucket, hash);

          pending = g_new0 (PendingOperation, 1);
          pending->request = request;
          pending->next = assets_downloaded;

          operation_started (request);

          /* An object is named after its own hash, so the bytes can be
             checked against it. */
          nuvashalauncher_task_scope_release_when_done (
            nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), url, destination, hash, G_CHECKSUM_SHA1,
                                     NULL, objects_downloaded, pending),
            request->cancellable);
        }
    }

  report (request);
  asset_index_load_free (load);
  operation_finished (request, assets_downloaded, NULL);
}

/* The index is on the disk: parse it in a worker thread and queue every
   object it names. */
static void
resolve_objects (ResolveRequest *request)
{
  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  AssetIndexLoad *load = g_new0 (AssetIndexLoad, 1);
  load->request = request;
  load->path = g_build_filename (request->indexes_dir, request->asset_index_id, NULL);
  load->objects_dir = g_strdup (request->objects_dir);

  operation_started (request);
  GTask *worker = g_task_new (NULL, request->cancellable, asset_index_parsed_ready, load);
  g_task_set_task_data (worker, load, NULL);
  g_task_run_in_thread (worker, asset_index_parse_thread);
  g_object_unref (worker);
}

void
resolve_assets (ResolveRequest *request)
{
  NuvashaLauncherVersionDescription *description = request->description;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;

  if (description->asset_index == NULL ||
      description->asset_index->url == NULL)
    {
      /* A version this old needs no assets at all. */
      resolve_finish (request);
      return;
    }

  path = g_build_filename (request->indexes_dir, request->asset_index_id, NULL);

  if (!nuvashalauncher_resolve_ensure_parent_directory (path, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      resolve_finish (request);
      return;
    }

  request->stage = "assets";
  request->done = 0;
  request->total = 1;

  download_file (request, resolve_objects, description->asset_index->url, path,
                 description->asset_index->sha1);
  report (request);
}

/* ---------------------------------------------------------- *
 * Entry point
 * ---------------------------------------------------------- */

/* Read logging and asset-index metadata from the resolved document. */
