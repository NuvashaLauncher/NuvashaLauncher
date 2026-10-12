/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* File discovery, hashing, and enabled/disabled filename handling for an
 * instance's content folder. This is independent of the page widgets. */

#include "aml-content-page.h"
#include "aml-content-index.h"
#include "aml-download.h"
#include "aml-task-private.h"
#include "aml-fs.h"

#include <archive.h>
#include <archive_entry.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * The files of the instance folder
 * ---------------------------------------------------------- */

void
aml_content_file_free (AmlContentFile *file)
{
  if (file == NULL)
    return;

  g_free (file->path);
  g_free (file->filename);
  g_free (file->sha1);
  g_free (file->sha512);
  g_free (file->download_url);
  g_free (file->project_id);
  g_free (file->local_id);
  g_free (file->project_name);
  g_free (file->search_key);
  g_free (file->description);
  g_free (file->icon_url);
  g_free (file->local_icon_path);
  g_free (file->version_id);
  g_free (file->version_name);
  g_free (file->version_number);
  g_clear_pointer (&file->required_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->optional_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->incompatible_dependencies, g_ptr_array_unref);
  g_free (file);
}

void
aml_content_file_refresh_search_key (AmlContentFile *file)
{
  const char *title = file->project_name != NULL && *file->project_name != '\0'
                        ? file->project_name : file->filename;
  g_autofree char *title_folded = g_utf8_casefold (title != NULL ? title : "",
                                                   -1);
  g_autofree char *file_folded =
    g_utf8_casefold (file->filename != NULL ? file->filename : "", -1);

  g_free (file->search_key);
  file->search_key = g_strconcat (title_folded != NULL ? title_folded : "",
                                  " ", file_folded != NULL ? file_folded : "",
                                  NULL);
}

char *
aml_content_folder_path (const char *instance_directory, AmlContentType type)
{
  g_return_val_if_fail (instance_directory != NULL, NULL);

  return g_build_filename (instance_directory,
                           aml_content_type_folder (type), NULL);
}

char *
aml_content_disabled_name (const char *filename, gboolean enabled)
{
  const char *suffix = AML_CONTENT_DISABLED_SUFFIX;
  gsize length = 0;

  g_return_val_if_fail (filename != NULL, NULL);

  length = strlen (filename);

  if (enabled)
    {
      if (length > strlen (suffix) && g_str_has_suffix (filename, suffix))
        return g_strndup (filename, length - strlen (suffix));

      return g_strdup (filename);
    }

  if (g_str_has_suffix (filename, suffix))
    return g_strdup (filename);

  return g_strconcat (filename, suffix, NULL);
}

char *
aml_content_set_disabled (const char *path, gboolean disabled, GError **error)
{
  g_autofree char *directory = NULL;
  g_autofree char *base = NULL;
  g_autofree char *name = NULL;
  g_autofree char *target = NULL;
  g_autoptr (GFile) from = NULL;
  g_autoptr (GFile) to = NULL;

  g_return_val_if_fail (path != NULL, NULL);

  directory = g_path_get_dirname (path);
  base = g_path_get_basename (path);
  name = aml_content_disabled_name (base, disabled);
  target = g_build_filename (directory, name, NULL);

  if (g_strcmp0 (path, target) == 0)
    return g_steal_pointer (&target);

  from = g_file_new_for_path (path);
  to = g_file_new_for_path (target);

  /* A rename of a local file is one system call: it waits for nothing
   * and moves no byte of the file itself. */
  if (!aml_fs_move_file (from, to, G_FILE_COPY_NONE, NULL, error))
    return NULL;

  return g_steal_pointer (&target);
}

gboolean
aml_content_remove_file (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return aml_fs_unlink (path, error);
}

gboolean
aml_content_ensure_folder (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return aml_fs_make_directory (path, 0755, error);
}

void
aml_content_delete_async (GFile *file, int io_priority,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  aml_fs_delete_async (file, io_priority, cancellable, callback, user_data);
}

gboolean
aml_content_delete_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return aml_fs_delete_finish (file, result, error);
}

/* TRUE when @name is a file of this content, suffix or not: the loaders
 * and the game only pick up the ones that do not carry the suffix. */
static gboolean
name_is_content (const char *name, const char *extension)
{
  g_autofree char *plain = aml_content_disabled_name (name, TRUE);

  return g_str_has_suffix (plain, extension);
}

/* The scan: what the folder holds and the hash of each file, which is
 * what Modrinth is then asked about. */
typedef struct
{
  /* Borrowed: the caller's handle keeps the operation alive, and every
     path that frees this run has already answered it. */
  AmlTaskScope *scope;
  GFile *folder;
  GFileEnumerator *enumerator; /* while the batches are read */
  char *instance_directory;
  AmlContentType type;
  char *extension;
  GPtrArray *files; /* AmlContentFile* */
  GPtrArray *index_entries; /* AmlContentIndexEntry* */
  guint index;
  guint hash_pending;
  gboolean hash_cancelled;
  guint local_pending;
  gboolean local_cancelled;

  /* The index load that has to finish before the scan can move on. */
  AmlTaskScope *index_operation;
} ScanRun;

static void scan_hash_next (ScanRun *run);
static void batch_read_next (ScanRun *run);
static int compare_files (gconstpointer a, gconstpointer b);
static void scan_local_metadata (ScanRun *run);
static void scan_free (ScanRun *run);

typedef struct {
  ScanRun *run;
  AmlContentFile *file;
} HashInput;

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
  start = strchr (start, '{');
  if (start == NULL)
    return;
  end = strchr (start, '}');
  if (end == NULL)
    end = (const char *) data + length;
  for (const char *cursor = start; cursor < end; cursor++)
    if (*cursor == '"')
      {
        const char *close = strchr (cursor + 1, '"');
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
          gint size = (gint) g_ascii_strtoll (name, NULL, 10);
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

static void
merge_dependencies (GPtrArray **destination, GPtrArray *source)
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
apply_local_json (AmlContentFile *file, const char *name,
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
            char *name = json_string_member (metadata, "name");
            char *description = json_string_member (metadata, "description");
            if (name != NULL)
              {
                g_free (file->project_name);
                file->project_name = name;
              }
            else
              g_free (name);
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
apply_local_mcmod (AmlContentFile *file, const guint8 *data, gsize length,
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
apply_local_toml (AmlContentFile *file, const guint8 *data, gsize length,
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
apply_resource_metadata (AmlContentFile *file)
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
                                                      "AML", "icons", NULL);
      g_autofree char *name = g_strconcat (file->sha1, ".png", NULL);
      g_autofree char *path = g_build_filename (directory, name, NULL);
      if (aml_fs_make_directory (directory, 0755, NULL) &&
          aml_fs_replace_contents (path, icon->data, icon->len, 0644, NULL))
        file->local_icon_path = g_steal_pointer (&path);
    }
  file->local_metadata = TRUE;
}

static void
apply_local_metadata (AmlContentFile *file, AmlContentType type)
{
  if (file->required_dependencies == NULL)
  if (file->required_dependencies == NULL)
    file->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (file->optional_dependencies == NULL)
    file->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (file->incompatible_dependencies == NULL)
    file->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  if (type == AML_CONTENT_RESOURCE_PACK)
    {
      g_clear_pointer (&file->project_name, g_free);
      apply_resource_metadata (file);
      return;
    }
  if (type == AML_CONTENT_SHADER_PACK)
    {
      g_clear_pointer (&file->project_name, g_free);
      file->local_metadata = TRUE;
      return;
    }
  char *icon_member = NULL;
  g_autoptr (GByteArray) icon_data = NULL;
  g_autoptr (GByteArray) metadata = NULL;

  /* Read the declared metadata files directly. This avoids depending on ZIP
   * entry order and ensures Forge/NeoForge sections are parsed reliably. */
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

  if (icon_member != NULL)
    icon_data = read_archive_member (file->path, icon_member);

  if (icon_data != NULL && file->sha1 != NULL)
    {
      g_autofree char *directory = g_build_filename (g_get_user_cache_dir (),
                                                      "AML", "icons", NULL);
      g_autofree char *name = g_strconcat (file->sha1, ".png", NULL);
      g_autofree char *path = g_build_filename (directory, name, NULL);
      if (aml_fs_make_directory (directory, 0755, NULL) &&
          aml_fs_replace_contents (path, icon_data->data, icon_data->len,
                                   0644, NULL))
        file->local_icon_path = g_steal_pointer (&path);
    }
  g_free (icon_member);
}

static void
scan_finish (ScanRun *run)
{
  g_ptr_array_sort (run->files, compare_files);
  aml_task_scope_return_pointer (run->scope, g_steal_pointer (&run->files),
                                 (GDestroyNotify) g_ptr_array_unref);
  scan_free (run);
}

static void
local_metadata_thread (GTask *task, gpointer source G_GNUC_UNUSED,
                       gpointer task_data,
                       GCancellable *cancellable G_GNUC_UNUSED)
{
  ScanRun *run = task_data;
  AmlTaskScope *scope = g_object_get_data (G_OBJECT (task),
                                           "aml-task-scope");

  for (guint i = 0; i < run->files->len; i++)
    {
      if (aml_task_scope_return_error_if_cancelled (scope))
        return;
      AmlContentFile *file = g_ptr_array_index (run->files, i);

      /* The content index already carries the remote identity and metadata
       * for files seen before. Reopening every archive just to rediscover
       * that information makes large modpacks needlessly expensive; only
       * inspect files that still need local identification. */
      if (file->project_id == NULL && !file->local_metadata)
        apply_local_metadata (file, run->type);
      aml_content_file_refresh_search_key (file);
    }

  aml_task_scope_return_boolean (scope, TRUE);
}

static void
local_metadata_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      scan_free (run);
      return;
    }

  scan_finish (run);
}

static void
scan_local_metadata (ScanRun *run)
{
  if (run->files->len == 0)
    {
      scan_finish (run);
      return;
    }
  GTask *task = g_task_new (NULL, aml_task_scope_get_cancellable (run->scope),
                            local_metadata_done, run);
  aml_task_scope_attach_task (task);
  g_task_set_task_data (task, run, NULL);
  g_task_run_in_thread (task, local_metadata_thread);
  g_object_unref (task);
}

static void
scan_free (ScanRun *run)
{
  g_clear_object (&run->enumerator);
  g_clear_object (&run->folder);
  if (run->index_operation != NULL)
    aml_task_scope_unref (run->index_operation);
  g_clear_pointer (&run->files, g_ptr_array_unref);
  g_clear_pointer (&run->index_entries, g_ptr_array_unref);
  g_free (run->instance_directory);
  g_free (run->extension);
  g_free (run);
}

static AmlContentIndexEntry *
scan_index_entry (ScanRun *run, AmlContentFile *file)
{
  g_autofree char *name = g_path_get_basename (file->path);

  for (guint i = 0; run->index_entries != NULL &&
                     i < run->index_entries->len; i++)
    {
      AmlContentIndexEntry *entry = g_ptr_array_index (run->index_entries, i);
      if (g_strcmp0 (entry->filename, name) == 0 &&
          g_strcmp0 (entry->hash, file->sha1) == 0)
        return entry;
    }
  return NULL;
}

static void
apply_index_entry (ScanRun *run, AmlContentFile *file)
{
  AmlContentIndexEntry *entry = scan_index_entry (run, file);

  if (entry == NULL)
    return;
  if (entry->project_id != NULL && *entry->project_id != '\0')
    file->project_id = g_strdup (entry->project_id);
  if (entry->hash512 != NULL && *entry->hash512 != '\0')
    file->sha512 = g_strdup (entry->hash512);
  if (entry->download_url != NULL && *entry->download_url != '\0')
    file->download_url = g_strdup (entry->download_url);
  if (!file->local_metadata && entry->project_name != NULL &&
      *entry->project_name != '\0')
    {
      g_free (file->project_name);
      file->project_name = g_strdup (entry->project_name);
    }
  if (entry->icon_url != NULL && *entry->icon_url != '\0')
    file->icon_url = g_strdup (entry->icon_url);
  if (entry->version_id != NULL && *entry->version_id != '\0')
    file->version_id = g_strdup (entry->version_id);
  if (entry->version_name != NULL && *entry->version_name != '\0')
    file->version_name = g_strdup (entry->version_name);
  merge_dependencies (&file->required_dependencies,
                      entry->required_dependencies);
  merge_dependencies (&file->optional_dependencies,
                      entry->optional_dependencies);
  merge_dependencies (&file->incompatible_dependencies,
                      entry->incompatible_dependencies);
}

/* One batch of the folder, then the next, so that a folder with a
 * thousand files in it is read without the main loop waiting. */
static void
batch_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autolist (GFileInfo) children = NULL;

  children = g_file_enumerator_next_files_finish (run->enumerator, result,
                                                  &error);

  if (g_cancellable_is_cancelled (aml_task_scope_get_cancellable (run->scope)))
    {
      scan_free (run);
      return;
    }

  /* No list and no error is how an enumeration says it has read the
   * whole folder. */
  if (children == NULL && error != NULL)
    {
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      scan_free (run);
      return;
    }

  for (GList *item = children; item != NULL; item = item->next)
    {
      GFileInfo *info = item->data;
      /* Borrowed from @info, which is dropped with the list. */
      const char *name = g_file_info_get_name (info);
      g_autoptr (GFile) child = NULL;
      AmlContentFile *file = NULL;

      if (name == NULL ||
          g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
        continue;

      if (!name_is_content (name, run->extension))
        continue;

      child = g_file_get_child (run->folder, name);
      file = g_new0 (AmlContentFile, 1);
      file->path = g_file_get_path (child);
      file->filename = aml_content_disabled_name (name, TRUE);
      file->enabled = g_strcmp0 (name, file->filename) == 0;
      g_ptr_array_add (run->files, file);
    }

  if (children != NULL)
    {
      batch_read_next (run);
      return;
    }

  scan_hash_next (run);
}

static void
batch_read_next (ScanRun *run)
{
  g_file_enumerator_next_files_async (run->enumerator, 64, G_PRIORITY_DEFAULT,
                                       aml_task_scope_get_cancellable (run->scope),
                                      batch_ready, run);
}

static void
scan_children_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileEnumerator) enumerator = NULL;

  enumerator = g_file_enumerate_children_finish (G_FILE (source), result,
                                                 &error);

  if (enumerator == NULL)
    {
       aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      scan_free (run);
      return;
    }

  run->enumerator = g_steal_pointer (&enumerator);
  batch_read_next (run);
}

static void
scan_index_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) entries =
    aml_content_index_load_finish (result, &error);

  if (run->index_operation != NULL)
    {
      aml_task_scope_unref (run->index_operation);
      run->index_operation = NULL;
    }

  /* A missing or damaged index is deliberately not fatal: the existing
   * hash lookup repairs what it can, and the folder remains usable offline. */
  if (entries == NULL)
    {
      g_clear_error (&error);
      entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify) aml_content_index_entry_free);
    }
  run->index_entries = g_steal_pointer (&entries);

  g_file_enumerate_children_async (
    run->folder,
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
     aml_task_scope_get_cancellable (run->scope), scan_children_ready, run);
}

static void
scan_hash_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  HashInput *input = user_data;
  ScanRun *run = input->run;
  AmlContentFile *file = input->file;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = NULL;

  digest = aml_download_file_hash_finish (result, &error);

   if (g_cancellable_is_cancelled (aml_task_scope_get_cancellable (run->scope)))
    run->hash_cancelled = TRUE;

  if (digest != NULL)
    {
      file->sha1 = g_steal_pointer (&digest);
      apply_index_entry (run, file);
    }
  else
    {
      /* A file that cannot be read is still a file that is there: it is
       * listed under its own name, without an answer from Modrinth. */
      g_clear_error (&error);
    }

  g_free (input);
  run->hash_pending--;
  if (run->hash_pending == 0)
    {
      if (run->hash_cancelled)
        {
           aml_task_scope_return_error (
             run->scope,
             g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                  "cancelled"));
          scan_free (run);
        }
      else
        scan_local_metadata (run);
    }
}

static void
scan_hash_next (ScanRun *run)
{
  run->hash_pending = run->files->len;
  if (run->hash_pending == 0)
    {
      scan_local_metadata (run);
      return;
    }
  for (guint i = 0; i < run->files->len; i++)
    {
      AmlContentFile *file = g_ptr_array_index (run->files, i);
      g_autoptr (GFile) handle = g_file_new_for_path (file->path);
      HashInput *input = g_new (HashInput, 1);
      input->run = run;
      input->file = file;
      aml_task_scope_release_when_done (
          aml_download_file_hash_async (handle, G_CHECKSUM_SHA1,
                                        scan_hash_ready, input),
          aml_task_scope_get_cancellable (run->scope));
    }
}

static int
compare_files (gconstpointer a, gconstpointer b)
{
  const AmlContentFile *left = *(const AmlContentFile *const *) a;
  const AmlContentFile *right = *(const AmlContentFile *const *) b;

  return g_utf8_collate (left->filename, right->filename);
}

AmlTaskScope *
aml_content_scan_async (const char *instance_directory, AmlContentType type,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  ScanRun *run = NULL;
  g_autofree char *folder = NULL;

  g_return_val_if_fail (instance_directory != NULL, NULL);

  folder = aml_content_folder_path (instance_directory, type);
  run = g_new0 (ScanRun, 1);
  run->scope = scope;
  aml_task_scope_set_check_cancellable (run->scope, FALSE);
  run->folder = g_file_new_for_path (folder);
  run->instance_directory = g_strdup (instance_directory);
  run->type = type;
  run->extension = g_strdup (aml_content_type_extension (type));
  run->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_content_file_free);

  /* An instance that never installed anything of this kind has no such
   * folder, which reads the same as an empty one. */
  if (!g_file_query_exists (run->folder, NULL))
    {
      aml_task_scope_return_pointer_deferred (scope,
                                              g_steal_pointer (&run->files),
                                              (GDestroyNotify) g_ptr_array_unref);
      scan_free (run);
      return g_steal_pointer (&scope);
    }

  run->index_operation = aml_content_index_load_async (instance_directory, type,
                                                       scan_index_ready, run);

  return g_steal_pointer (&scope);
}

GPtrArray *
aml_content_scan_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
