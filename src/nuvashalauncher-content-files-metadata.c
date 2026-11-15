/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Local metadata extraction for installed content. Directory scanning and
 * hashing remain in nuvashalauncher-content-files.c. */

#include "nuvashalauncher-content-page.h"
#include "nuvashalauncher-content-files-private.h"
#include "nuvashalauncher-task-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-icons.h"

#include <archive.h>
#include <archive_entry.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define MAX_LOCAL_ARCHIVE_MEMBER_SIZE (1024 * 1024)
#define MAX_LOCAL_ARCHIVE_ENTRIES 10000
#include <json-glib/json-glib.h>
#include <string.h>

static char *
json_string_member (JsonObject *object, const char *name)
{
  const char *value = json_object_get_string_member_with_default (object, name,
                                                                   NULL);
  return value != NULL ? g_strdup (value) : NULL;
}

static char *
loose_json_string (const guint8 *data, gsize length, const char *name)
{
  g_autofree char *needle = g_strdup_printf ("\"%s\"", name);
  const char *start = g_strstr_len ((const char *) data, length, needle);
  const char *end = NULL;

  if (start == NULL)
    return NULL;
  start += strlen (needle);
  while ((gsize) (start - (const char *) data) < length &&
         g_ascii_isspace (*start))
    start++;
  if ((gsize) (start - (const char *) data) >= length || *start != ':')
    return NULL;
  start++;
  while ((gsize) (start - (const char *) data) < length &&
         g_ascii_isspace (*start))
    start++;
  if ((gsize) (start - (const char *) data) >= length || *start != '"')
    return NULL;
  start++;
  for (end = start; (gsize) (end - (const char *) data) < length; end++)
    {
      /* A backslash escapes the next character, including another backslash. */
      if (*end == '\\')
        {
          end++;
          continue;
        }
      if (*end == '"')
        return g_strndup (start, end - start);
    }
  return NULL;
}

static void
loose_json_dependencies (const guint8 *data, gsize length,
                          GPtrArray *dependencies)
{
  const char *start = g_strstr_len ((const char *) data, length, "\"depends\"");
  const char *end = NULL;

  if (start == NULL || dependencies == NULL)
    return;
  start = (const char *) memchr (
    start, '{', length - (gsize) (start - (const char *) data));
  if (start == NULL)
    return;
  end = (const char *) memchr (
    start, '}', length - (gsize) (start - (const char *) data));
  if (end == NULL)
    end = (const char *) data + length;
  for (const char *cursor = start; cursor < end; cursor++)
    if (*cursor == '"')
      {
        const char *close = (const char *) memchr (
          cursor + 1, '"', end - (cursor + 1));
        const char *colon = close != NULL ? close + 1 : NULL;
        if (close == NULL || close >= end)
          break;
        while (colon < end && g_ascii_isspace (*colon))
          colon++;
        if (colon < end && *colon == ':')
          {
            g_autofree char *dependency =
              g_strndup (cursor + 1, close - cursor - 1);
            if (g_strcmp0 (dependency, "minecraft") != 0 &&
                g_strcmp0 (dependency, "fabricloader") != 0 &&
                g_strcmp0 (dependency, "java") != 0)
              g_ptr_array_add (dependencies, g_steal_pointer (&dependency));
          }
        cursor = close;
      }
}

static JsonObject *
json_object_member (JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member (object, name);

  return node != NULL && JSON_NODE_HOLDS_OBJECT (node)
           ? json_node_get_object (node) : NULL;
}

static char *
json_icon_member (JsonObject *object)
{
  JsonNode *node = json_object_get_member (object, "icon");

  if (node == NULL)
    return NULL;
  if (JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    return json_node_dup_string (node);
  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      const char *best = NULL;
      gint best_size = -1;
      GList *members = json_object_get_members (json_node_get_object (node));

      for (GList *item = members; item != NULL; item = item->next)
        {
          const char *name = item->data;
          char *end = NULL;
          gint64 parsed = g_ascii_strtoll (name, &end, 10);

          if (end == name || *end != '\0' || parsed < 0 ||
              parsed > G_MAXINT)
            continue;

          gint size = (gint) parsed;
          if (size > best_size)
            {
              best = name;
              best_size = size;
            }
        }
      if (best != NULL)
        {
          char *value = g_strdup (json_object_get_string_member_with_default (
            json_node_get_object (node), best, NULL));
          g_list_free (members);
          return value;
        }
      g_list_free (members);
    }
  return NULL;
}

static void
local_dependency_keys (JsonObject *object, GPtrArray *dependencies)
{
  if (object == NULL || dependencies == NULL)
    return;
  GList *members = json_object_get_members (object);

  for (GList *item = members; item != NULL; item = item->next)
    {
      const char *name = item->data;
      if (g_strcmp0 (name, "minecraft") != 0 &&
          g_strcmp0 (name, "fabricloader") != 0 &&
          g_strcmp0 (name, "java") != 0)
        g_ptr_array_add (dependencies, g_strdup (name));
    }
  g_list_free (members);
}

static void
local_dependency_node (JsonObject *object, const char *name,
                       GPtrArray *dependencies)
{
  JsonNode *node = json_object_get_member (object, name);

  if (node == NULL)
    return;
  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      local_dependency_keys (json_node_get_object (node), dependencies);
      return;
    }
  if (JSON_NODE_HOLDS_ARRAY (node))
    for (guint i = 0; i < json_array_get_length (json_node_get_array (node)); i++)
      {
        JsonNode *item = json_array_get_element (json_node_get_array (node), i);
        const char *value = NULL;
        if (JSON_NODE_HOLDS_VALUE (item) &&
            json_node_get_value_type (item) == G_TYPE_STRING)
          value = json_node_get_string (item);
        else if (JSON_NODE_HOLDS_OBJECT (item))
          value = json_object_get_string_member_with_default (
            json_node_get_object (item), "id", NULL);
        if (value != NULL && *value != '\0')
          g_ptr_array_add (dependencies, g_strdup (value));
      }
}

void
nuvashalauncher_content_merge_dependencies (GPtrArray **destination, GPtrArray *source)
{
  if (source == NULL || source->len == 0)
    return;
  if (*destination == NULL)
    *destination = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < source->len; i++)
    {
      const char *value = g_ptr_array_index (source, i);
      gboolean found = FALSE;
      for (guint j = 0; j < (*destination)->len; j++)
        if (g_strcmp0 (value, g_ptr_array_index (*destination, j)) == 0)
          {
            found = TRUE;
            break;
          }
      if (!found)
        g_ptr_array_add (*destination, g_strdup (value));
    }
}

static char *
toml_string (const char *contents, const char *key)
{
  const char *cursor = contents;
  gsize key_length = strlen (key);

  while ((cursor = g_strstr_len (cursor, -1, key)) != NULL)
    {
      const char *start = cursor + key_length;
      const char *end = NULL;
      char quote = '\0';

      if ((cursor != contents &&
           (g_ascii_isalnum (cursor[-1]) || cursor[-1] == '_')) ||
          (g_ascii_isalnum (*start) || *start == '_'))
        {
          cursor = start;
          continue;
        }
      while (g_ascii_isspace (*start))
        start++;
      if (*start != '=')
        {
          cursor += key_length;
          continue;
        }
      start++;
      while (g_ascii_isspace (*start))
        start++;
      if (*start != '\"' && *start != '\'')
        {
          cursor += key_length;
          continue;
        }
      quote = *start++;
      end = strchr (start, quote);
      return end != NULL ? g_strndup (start, end - start) : NULL;
    }
  return NULL;
}

static gboolean
archive_entry_bytes (struct archive *archive, struct archive_entry *header,
                     GByteArray **out)
{
  la_int64_t size = archive_entry_size (header);
  const void *block = NULL;
  size_t block_size = 0;
  la_int64_t offset = 0;
  GByteArray *bytes = g_byte_array_new ();

  if (archive_entry_size_is_set (header) &&
      (size < 0 || size > (la_int64_t) MAX_LOCAL_ARCHIVE_MEMBER_SIZE))
    {
      g_byte_array_unref (bytes);
      return FALSE;
    }
  for (;;)
    {
      int status = archive_read_data_block (archive, &block, &block_size, &offset);

      if (status == ARCHIVE_EOF)
        break;
      if (status != ARCHIVE_OK ||
          block_size > MAX_LOCAL_ARCHIVE_MEMBER_SIZE - bytes->len)
        {
          g_byte_array_unref (bytes);
          return FALSE;
        }

      g_byte_array_append (bytes, block, block_size);
    }

  *out = bytes;
  return TRUE;
}

static void
read_archive_members (const char *path, const char * const *members,
                      gsize count, GByteArray **out_results)
{
  struct archive *archive = archive_read_new ();
  struct archive_entry *header = NULL;
  guint entries = 0;
  gsize remaining = count;
  int status = ARCHIVE_EOF;

  for (gsize i = 0; i < count; i++)
    out_results[i] = NULL;

  archive_read_support_filter_all (archive);
  archive_read_support_format_zip (archive);
  if (archive_read_open_filename (archive, path, 64 * 1024) != ARCHIVE_OK)
    {
      archive_read_free (archive);
      return;
    }

  while (remaining > 0 &&
         ((status = archive_read_next_header (archive, &header)) == ARCHIVE_OK ||
          status == ARCHIVE_WARN))
    {
      const char *entry_name;
      gboolean matched = FALSE;

      if (++entries > MAX_LOCAL_ARCHIVE_ENTRIES)
        break;

      entry_name = archive_entry_pathname (header);
      if (archive_entry_filetype (header) == AE_IFREG && entry_name != NULL)
        {
          for (gsize i = 0; i < count; i++)
            {
              if (out_results[i] == NULL && g_strcmp0 (entry_name, members[i]) == 0)
                {
                  if (archive_entry_bytes (archive, header, &out_results[i]) &&
                      out_results[i] != NULL)
                    remaining--;
                  matched = TRUE;
                  break;
                }
            }
        }
      if (!matched)
        archive_read_data_skip (archive);
    }

  if (status != ARCHIVE_EOF && status != ARCHIVE_OK && status != ARCHIVE_WARN)
    {
      for (gsize i = 0; i < count; i++)
        g_clear_pointer (&out_results[i], g_byte_array_unref);
    }

  archive_read_close (archive);
  archive_read_free (archive);
}

static char *
local_icon_cache_path (const char *sha1, char **out_directory)
{
  g_autofree char *directory = g_build_filename (
    g_get_user_cache_dir (), "nuvashalauncher", "icons", "local", NULL);
  g_autofree char *name = g_strconcat (sha1, ".png", NULL);
  char *path = g_build_filename (directory, name, NULL);

  if (out_directory != NULL)
    *out_directory = g_steal_pointer (&directory);
  return path;
}

static gboolean
write_cached_local_icon (const char *sha1, const guint8 *data, gsize len,
                         char **out_path, GError **error)
{
  g_autofree char *directory = NULL;
  g_autofree char *path = local_icon_cache_path (sha1, &directory);
  g_autoptr (GInputStream) stream = NULL;
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  g_autoptr (GdkPixbuf) scaled = NULL;
  g_autofree char *png_buf = NULL;
  gsize png_len = 0;
  int width = 0;
  int height = 0;

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      *out_path = g_steal_pointer (&path);
      return TRUE;
    }

  stream = g_memory_input_stream_new_from_data (data, (gssize) len, NULL);
  pixbuf = gdk_pixbuf_new_from_stream (stream, NULL, error);
  if (pixbuf == NULL)
    return FALSE;

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  if (width > NUVASHALAUNCHER_ICON_SIZE || height > NUVASHALAUNCHER_ICON_SIZE)
    {
      int target_w = NUVASHALAUNCHER_ICON_SIZE;
      int target_h = NUVASHALAUNCHER_ICON_SIZE;
      if (width > height)
        target_h = MAX (1, (int) ((gint64) height * NUVASHALAUNCHER_ICON_SIZE / width));
      else if (height > width)
        target_w = MAX (1, (int) ((gint64) width * NUVASHALAUNCHER_ICON_SIZE / height));
      scaled = gdk_pixbuf_scale_simple (pixbuf, target_w, target_h, GDK_INTERP_BILINEAR);
      if (scaled == NULL)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "the local icon could not be scaled");
          return FALSE;
        }
    }

  if (!gdk_pixbuf_save_to_buffer (scaled != NULL ? scaled : pixbuf,
                                  &png_buf, &png_len, "png", error, NULL))
    return FALSE;

  if (!nuvashalauncher_fs_make_directory (directory, 0755, error))
    return FALSE;

  if (!g_file_set_contents_full (path, png_buf, (gssize) png_len,
                                 G_FILE_SET_CONTENTS_NONE, 0644, error))
    return FALSE;

  *out_path = g_steal_pointer (&path);
  return TRUE;
}

static void
apply_local_json (NuvashaLauncherContentFile *file, const char *name,
                  const guint8 *data, gsize length, char **icon_member)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  JsonNode *root = NULL;
  JsonObject *object = NULL;

  if (!json_parser_load_from_data (parser, (const char *) data, length, &error))
    {
      if (g_strcmp0 (name, "fabric.mod.json") == 0)
        {
          g_free (file->project_name);
          g_free (file->version_name);
          file->project_name = loose_json_string (data, length, "name");
          file->local_id = loose_json_string (data, length, "id");
          file->version_name = loose_json_string (data, length, "version");
          *icon_member = loose_json_string (data, length, "icon");
          loose_json_dependencies (data, length, file->required_dependencies);
          file->local_metadata = file->project_name != NULL;
        }
      return;
    }
  root = json_parser_get_root (parser);
  if (g_strcmp0 (name, "quilt.mod.json") == 0)
    {
      if (!JSON_NODE_HOLDS_OBJECT (root))
        return;
      object = json_object_member (json_node_get_object (root),
                                   "quilt_loader");
      if (object == NULL)
        return;
      g_free (file->project_name);
      g_free (file->version_name);
      file->project_name = json_string_member (object, "id");
      file->local_id = json_string_member (object, "id");
      file->version_name = json_string_member (object, "version");
      {
        JsonObject *metadata = json_object_member (object, "metadata");
        if (metadata != NULL)
          {
            char *metadata_name = json_string_member (metadata, "name");
            char *description = json_string_member (metadata, "description");
            if (metadata_name != NULL)
              {
                g_free (file->project_name);
                file->project_name = metadata_name;
              }
            else
              g_free (metadata_name);
            file->description = description;
            *icon_member = json_icon_member (metadata);
          }
      }
      local_dependency_node (object, "depends", file->required_dependencies);
      file->local_metadata = TRUE;
      return;
    }
  if (!JSON_NODE_HOLDS_OBJECT (root))
    return;
  object = json_node_get_object (root);
  g_free (file->project_name);
  g_free (file->version_name);
  file->project_name = json_string_member (object, "name");
  file->local_id = json_string_member (object, "id");
  file->description = json_string_member (object, "description");
  file->version_name = json_string_member (object, "version");
  *icon_member = json_icon_member (object);
  local_dependency_node (object, "depends", file->required_dependencies);
  file->local_metadata = TRUE;
}

static void
apply_local_mcmod (NuvashaLauncherContentFile *file, const guint8 *data, gsize length,
                   char **icon_member)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  JsonNode *root = NULL;
  JsonObject *object = NULL;

  if (!json_parser_load_from_data (parser, (const char *) data, length, &error))
    return;
  root = json_parser_get_root (parser);
  if (JSON_NODE_HOLDS_ARRAY (root) &&
      json_array_get_length (json_node_get_array (root)) > 0)
    object = json_node_get_object (json_array_get_element (
      json_node_get_array (root), 0));
  else if (JSON_NODE_HOLDS_OBJECT (root))
    {
      JsonObject *root_object = json_node_get_object (root);
      JsonNode *mods = json_object_get_member (root_object, "modList");
      if (mods != NULL && JSON_NODE_HOLDS_ARRAY (mods) &&
          json_array_get_length (json_node_get_array (mods)) > 0)
        object = json_node_get_object (json_array_get_element (
          json_node_get_array (mods), 0));
      else
        object = root_object;
    }
  if (object == NULL)
    return;
  g_free (file->project_name);
  g_free (file->version_name);
  file->project_name = json_string_member (object, "name");
  file->local_id = json_string_member (object, "modid");
  file->description = json_string_member (object, "description");
  file->version_name = json_string_member (object, "version");
  *icon_member = json_string_member (object, "logoFile");
  file->local_metadata = TRUE;
}

static void
apply_local_toml (NuvashaLauncherContentFile *file, const guint8 *data, gsize length,
                  char **icon_member)
{
  g_autofree char *contents = g_strndup ((const char *) data, length);
  const char *mods = g_strstr_len (contents, -1, "[[mods]]");
  const char *next_section = NULL;
  g_autofree char *scope = NULL;
  const char *lookup = contents;
  g_autofree char *name = NULL;
  g_autofree char *id = NULL;
  g_autofree char *version = NULL;
  g_autofree char *description = NULL;
  g_autofree char *icon = NULL;

  if (mods != NULL)
    {
      next_section = g_strstr_len (mods + strlen ("[[mods]]"), -1, "[[");
      scope = next_section != NULL
                ? g_strndup (mods, next_section - mods) : g_strdup (mods);
      lookup = scope;
    }
  name = toml_string (lookup, "displayName");
  id = toml_string (lookup, "modId");
  version = toml_string (lookup, "version");
  description = toml_string (lookup, "description");
  /* Forge often puts logoFile before the [[mods]] table, while the name
   * fields live inside it. */
  icon = toml_string (contents, "logoFile");
  file->local_id = g_strdup (id);
  if (name == NULL)
    name = g_steal_pointer (&id);
  if (name != NULL)
    {
      g_free (file->project_name);
      file->project_name = g_steal_pointer (&name);
    }
  if (version != NULL)
    {
      g_free (file->version_name);
      file->version_name = g_steal_pointer (&version);
    }
  if (description != NULL)
    file->description = g_steal_pointer (&description);
  if (icon != NULL)
    {
      g_free (*icon_member);
      *icon_member = g_steal_pointer (&icon);
    }
  file->local_metadata = TRUE;
}

static void
apply_resource_metadata (NuvashaLauncherContentFile *file)
{
  static const char * const members[] = { "pack.mcmeta", "pack.png" };
  GByteArray *results[2] = { NULL, NULL };
  g_autoptr (GByteArray) metadata = NULL;
  g_autoptr (GByteArray) icon = NULL;
  g_autofree char *cached_icon = NULL;
  gsize query_count = 2;

  if (file->sha1 != NULL)
    {
      cached_icon = local_icon_cache_path (file->sha1, NULL);
      if (g_file_test (cached_icon, G_FILE_TEST_IS_REGULAR))
        {
          file->local_icon_path = g_steal_pointer (&cached_icon);
          query_count = 1;
        }
    }

  read_archive_members (file->path, members, query_count, results);
  metadata = results[0];
  icon = results[1];

  if (metadata != NULL)
    {
      g_autoptr (JsonParser) parser = json_parser_new ();
      if (json_parser_load_from_data (parser, (const char *) metadata->data,
                                      metadata->len, NULL))
        {
          JsonNode *root = json_parser_get_root (parser);
          JsonNode *pack = root != NULL && JSON_NODE_HOLDS_OBJECT (root)
                             ? json_object_get_member (
                                 json_node_get_object (root), "pack") : NULL;
          JsonNode *description = pack != NULL && JSON_NODE_HOLDS_OBJECT (pack)
                                    ? json_object_get_member (
                                        json_node_get_object (pack), "description")
                                    : NULL;
          if (description != NULL && JSON_NODE_HOLDS_VALUE (description) &&
              json_node_get_value_type (description) == G_TYPE_STRING)
            file->description = json_node_dup_string (description);
        }
    }
  if (icon != NULL && file->sha1 != NULL && file->local_icon_path == NULL)
    {
      g_autofree char *path = NULL;
      if (write_cached_local_icon (file->sha1, icon->data, icon->len, &path, NULL))
        file->local_icon_path = g_steal_pointer (&path);
    }
  file->local_metadata = TRUE;
}

typedef struct
{
  NuvashaLauncherTaskScope *scope;
  char *archive_path;
  char *member;
  char *sha1;
} LocalIconExtract;

static void
local_icon_extract_free (LocalIconExtract *extract)
{
  if (extract->scope != NULL)
    nuvashalauncher_task_scope_unref (extract->scope);
  g_free (extract->archive_path);
  g_free (extract->member);
  g_free (extract->sha1);
  g_free (extract);
}

static void
local_icon_extract_thread (GTask *task G_GNUC_UNUSED,
                           gpointer source_object G_GNUC_UNUSED,
                           gpointer task_data,
                           GCancellable *cancellable)
{
  LocalIconExtract *extract = task_data;
  g_autoptr (NuvashaLauncherTaskScope) scope = g_steal_pointer (&extract->scope);
  g_autoptr (GByteArray) icon = NULL;
  g_autofree char *cached_path = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      nuvashalauncher_task_scope_return_error (scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
      return;
    }

  cached_path = local_icon_cache_path (extract->sha1, NULL);
  if (g_file_test (cached_path, G_FILE_TEST_IS_REGULAR))
    {
      nuvashalauncher_task_scope_return_pointer (scope, g_steal_pointer (&cached_path), g_free);
      return;
    }

  read_archive_members (extract->archive_path, (const char * const *) &extract->member,
                        1, &icon);
  if (icon == NULL)
    {
      nuvashalauncher_task_scope_return_error (scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "the local icon was not found"));
      return;
    }

  if (!write_cached_local_icon (extract->sha1, icon->data, icon->len, &path, &error))
    {
      if (error == NULL)
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                     "the local icon could not be scaled");
      nuvashalauncher_task_scope_return_error (scope, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_task_scope_return_pointer (scope, g_steal_pointer (&path), g_free);
}

NuvashaLauncherTaskScope *
nuvashalauncher_content_load_local_icon_async (NuvashaLauncherContentFile *file,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  LocalIconExtract *extract = NULL;

  g_return_val_if_fail (file != NULL, NULL);

  if (file->local_icon_member == NULL || file->sha1 == NULL)
    {
      nuvashalauncher_task_scope_return_pointer_deferred (scope, NULL, NULL);
      return g_steal_pointer (&scope);
    }

  extract = g_new0 (LocalIconExtract, 1);
  extract->scope = nuvashalauncher_task_scope_ref (scope);
  extract->archive_path = g_strdup (file->path);
  extract->member = g_strdup (file->local_icon_member);
  extract->sha1 = g_strdup (file->sha1);
  g_task_set_task_data (nuvashalauncher_task_scope_task (scope), extract,
                        (GDestroyNotify) local_icon_extract_free);
  g_task_run_in_thread (nuvashalauncher_task_scope_task (scope), local_icon_extract_thread);

  return g_steal_pointer (&scope);
}

char *
nuvashalauncher_content_load_local_icon_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

void
nuvashalauncher_content_apply_local_metadata (NuvashaLauncherContentFile *file, NuvashaLauncherContentType type)
{
  if (file->required_dependencies == NULL)
    file->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (file->optional_dependencies == NULL)
    file->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (file->incompatible_dependencies == NULL)
    file->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (type == NUVASHALAUNCHER_CONTENT_RESOURCE_PACK)
    {
      apply_resource_metadata (file);
      return;
    }
  if (type == NUVASHALAUNCHER_CONTENT_SHADER_PACK)
    {
      file->local_metadata = TRUE;
      return;
    }
  static const char * const mod_members[] = {
    "fabric.mod.json",
    "quilt.mod.json",
    "META-INF/mods.toml",
    "META-INF/neoforge.mods.toml",
    "mcmod.info",
  };
  GByteArray *results[G_N_ELEMENTS (mod_members)] = { NULL };
  char *icon_member = NULL;

  /* Read all declared metadata candidates in a single archive pass. */
  read_archive_members (file->path, mod_members, G_N_ELEMENTS (mod_members), results);

  if (results[0] != NULL)
    apply_local_json (file, "fabric.mod.json", results[0]->data, results[0]->len,
                      &icon_member);
  else if (results[1] != NULL)
    apply_local_json (file, "quilt.mod.json", results[1]->data, results[1]->len,
                      &icon_member);
  else if (results[2] != NULL)
    apply_local_toml (file, results[2]->data, results[2]->len, &icon_member);
  else if (results[3] != NULL)
    apply_local_toml (file, results[3]->data, results[3]->len, &icon_member);
  else if (results[4] != NULL)
    apply_local_mcmod (file, results[4]->data, results[4]->len, &icon_member);

  for (gsize i = 0; i < G_N_ELEMENTS (results); i++)
    g_clear_pointer (&results[i], g_byte_array_unref);

  file->local_icon_member = icon_member;
}
