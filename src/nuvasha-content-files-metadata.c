/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Local metadata extraction for installed content. Directory scanning and
 * hashing remain in nuvasha-content-files.c. */

#include "nuvasha-content-page.h"
#include "nuvasha-content-files-private.h"
#include "nuvasha-task-private.h"
#include "nuvasha-fs.h"
#include "nuvasha-icons.h"

#include <archive.h>
#include <archive_entry.h>
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
    if (*end == '"' && (end == start || end[-1] != '\\'))
      return g_strndup (start, end - start);
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
nuvasha_content_merge_dependencies (GPtrArray **destination, GPtrArray *source)
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

  if (!archive_entry_size_is_set (header) || size < 0 || size > 1024 * 1024)
    {
      g_byte_array_unref (bytes);
      return FALSE;
    }
  while (archive_read_data_block (archive, &block, &block_size, &offset) ==
         ARCHIVE_OK)
    g_byte_array_append (bytes, block, block_size);
  *out = bytes;
  return TRUE;
}

static GByteArray *
read_archive_member (const char *path, const char *member)
{
  struct archive *archive = archive_read_new ();
  struct archive_entry *header = NULL;
  GByteArray *bytes = NULL;

  archive_read_support_format_zip (archive);
  if (archive_read_open_filename (archive, path, 64 * 1024) != ARCHIVE_OK)
    {
      archive_read_free (archive);
      return NULL;
    }
  while (archive_read_next_header (archive, &header) == ARCHIVE_OK)
    {
      if (g_strcmp0 (archive_entry_pathname (header), member) == 0 &&
          archive_entry_filetype (header) == AE_IFREG)
        {
          archive_entry_bytes (archive, header, &bytes);
          break;
        }
      archive_read_data_skip (archive);
    }
  archive_read_close (archive);
  archive_read_free (archive);

  return bytes;
}

static void
apply_local_json (NuvashaContentFile *file, const char *name,
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
apply_local_mcmod (NuvashaContentFile *file, const guint8 *data, gsize length,
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
apply_local_toml (NuvashaContentFile *file, const guint8 *data, gsize length,
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
apply_resource_metadata (NuvashaContentFile *file)
{
  g_autoptr (GByteArray) metadata =
    read_archive_member (file->path, "pack.mcmeta");
  g_autoptr (GByteArray) icon =
    read_archive_member (file->path, "pack.png");

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
  if (icon != NULL && file->sha1 != NULL)
    {
      g_autofree char *directory = g_build_filename (g_get_user_cache_dir (),
                                                      "nuvasha", "icons", "local", NULL);
      g_autofree char *name = g_strconcat (file->sha1, ".png", NULL);
      g_autofree char *path = g_build_filename (directory, name, NULL);
      if (nuvasha_fs_make_directory (directory, 0755, NULL) &&
          nuvasha_fs_replace_contents (path, icon->data, icon->len, 0644, NULL) &&
          nuvasha_icon_scale_file (path))
        file->local_icon_path = g_steal_pointer (&path);
    }
  file->local_metadata = TRUE;
}

gboolean
nuvasha_content_load_local_icon (NuvashaContentFile *file)
{
  g_autoptr (GByteArray) icon = NULL;
  g_autofree char *directory = NULL;
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;

  if (file == NULL || file->local_icon_member == NULL ||
      file->local_icon_path != NULL || file->sha1 == NULL)
    return file != NULL && file->local_icon_path != NULL;

  icon = read_archive_member (file->path, file->local_icon_member);
  if (icon == NULL)
    return FALSE;

  directory = g_build_filename (g_get_user_cache_dir (), "nuvasha", "icons",
                                "local", NULL);
  name = g_strconcat (file->sha1, ".png", NULL);
  path = g_build_filename (directory, name, NULL);

  if (!nuvasha_fs_make_directory (directory, 0755, NULL) ||
      !nuvasha_fs_replace_contents (path, icon->data, icon->len, 0644, NULL) ||
      !nuvasha_icon_scale_file (path))
    return FALSE;

  file->local_icon_path = g_steal_pointer (&path);
  return TRUE;
}

typedef struct
{
  NuvashaTaskScope *scope;
  char *archive_path;
  char *member;
  char *sha1;
} LocalIconExtract;

static void
local_icon_extract_free (LocalIconExtract *extract)
{
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
  g_autoptr (GByteArray) icon = NULL;
  g_autofree char *directory = NULL;
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      nuvasha_task_scope_return_error (extract->scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
      return;
    }

  icon = read_archive_member (extract->archive_path, extract->member);
  if (icon == NULL)
    {
      nuvasha_task_scope_return_error (extract->scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "the local icon was not found"));
      return;
    }

  directory = g_build_filename (g_get_user_cache_dir (), "nuvasha", "icons",
                                "local", NULL);
  name = g_strconcat (extract->sha1, ".png", NULL);
  path = g_build_filename (directory, name, NULL);

  if (!nuvasha_fs_make_directory (directory, 0755, &error) ||
      !nuvasha_fs_replace_contents (path, icon->data, icon->len, 0644, &error) ||
      !nuvasha_icon_scale_file (path))
    {
      if (error == NULL)
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                     "the local icon could not be scaled");
      nuvasha_task_scope_return_error (extract->scope, g_steal_pointer (&error));
      return;
    }

  nuvasha_task_scope_return_pointer (extract->scope, g_steal_pointer (&path), g_free);
}

NuvashaTaskScope *
nuvasha_content_load_local_icon_async (NuvashaContentFile *file,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  LocalIconExtract *extract = NULL;

  g_return_val_if_fail (file != NULL, NULL);

  if (file->local_icon_member == NULL || file->sha1 == NULL)
    {
      nuvasha_task_scope_return_pointer_deferred (scope, NULL, NULL);
      return g_steal_pointer (&scope);
    }

  extract = g_new0 (LocalIconExtract, 1);
  extract->scope = scope;
  extract->archive_path = g_strdup (file->path);
  extract->member = g_strdup (file->local_icon_member);
  extract->sha1 = g_strdup (file->sha1);
  g_task_set_task_data (nuvasha_task_scope_task (scope), extract,
                        (GDestroyNotify) local_icon_extract_free);
  g_task_run_in_thread (nuvasha_task_scope_task (scope), local_icon_extract_thread);

  return g_steal_pointer (&scope);
}

char *
nuvasha_content_load_local_icon_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

void
nuvasha_content_apply_local_metadata (NuvashaContentFile *file, NuvashaContentType type)
{
  if (file->required_dependencies == NULL)
    file->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (file->optional_dependencies == NULL)
    file->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (file->incompatible_dependencies == NULL)
    file->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (type == nuvasha_CONTENT_RESOURCE_PACK)
    {
      g_clear_pointer (&file->project_name, g_free);
      apply_resource_metadata (file);
      return;
    }
  if (type == nuvasha_CONTENT_SHADER_PACK)
    {
      g_clear_pointer (&file->project_name, g_free);
      file->local_metadata = TRUE;
      return;
    }
  char *icon_member = NULL;
  g_autoptr (GByteArray) metadata = NULL;

  /* Read the declared metadata files directly so Forge/NeoForge sections are
   * parsed reliably. */
  metadata = read_archive_member (file->path, "fabric.mod.json");
  if (metadata != NULL)
    apply_local_json (file, "fabric.mod.json", metadata->data, metadata->len,
                      &icon_member);
  else
    {
      metadata = read_archive_member (file->path, "quilt.mod.json");
      if (metadata != NULL)
        apply_local_json (file, "quilt.mod.json", metadata->data, metadata->len,
                          &icon_member);
    }
  if (!file->local_metadata)
    {
      g_clear_pointer (&metadata, g_byte_array_unref);
      metadata = read_archive_member (file->path, "META-INF/mods.toml");
      if (metadata == NULL)
        metadata = read_archive_member (file->path,
                                        "META-INF/neoforge.mods.toml");
      if (metadata != NULL)
        apply_local_toml (file, metadata->data, metadata->len, &icon_member);
    }
  if (!file->local_metadata && metadata == NULL)
    {
      metadata = read_archive_member (file->path, "mcmod.info");
      if (metadata != NULL)
        apply_local_mcmod (file, metadata->data, metadata->len, &icon_member);
    }

  file->local_icon_member = icon_member;
}
