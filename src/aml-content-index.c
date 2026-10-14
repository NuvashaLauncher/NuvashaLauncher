/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-index.h"
#include "aml-task-private.h"
#include "aml-fs.h"


#include "aml-content-page.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#define INDEX_DIRECTORY ".index"
#define INDEX_PROVIDER "Modrinth"

static char *
index_directory (const char *instance_directory, AmlContentType type)
{
  g_autofree char *folder = aml_content_folder_path (instance_directory, type);

  return g_build_filename (folder, INDEX_DIRECTORY, NULL);
}

char *
aml_content_index_entry_path (const char *instance_directory,
                              AmlContentType type, const char *filename)
{
  g_autofree char *directory = index_directory (instance_directory, type);
  g_autofree char *key = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
                                                         filename, -1);
  g_autofree char *name = g_strconcat (key, ".json", NULL);

  return g_build_filename (directory, name, NULL);
}

GQuark
aml_content_index_error_quark (void)
{
  return g_quark_from_static_string ("aml-content-index-error");
}

void
aml_content_index_entry_free (AmlContentIndexEntry *entry)
{
  if (entry == NULL)
    return;

  g_free (entry->filename);
  g_free (entry->hash);
  g_free (entry->hash512);
  g_free (entry->download_url);
  g_free (entry->env_client);
  g_free (entry->env_server);
  g_free (entry->provider);
  g_free (entry->project_id);
  g_free (entry->project_name);
  g_free (entry->icon_url);
  g_free (entry->local_icon_member);
  g_free (entry->local_icon_path);
  g_free (entry->version_id);
  g_free (entry->version_name);
  g_clear_pointer (&entry->required_dependencies, g_ptr_array_unref);
  g_clear_pointer (&entry->optional_dependencies, g_ptr_array_unref);
  g_clear_pointer (&entry->incompatible_dependencies, g_ptr_array_unref);
  g_free (entry);
}

static GPtrArray *
empty_entries (void)
{
  return g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_content_index_entry_free);
}

static void
read_dependencies (JsonObject *object, AmlContentIndexEntry *entry)
{
  const char *names[] = { "required_dependencies", "optional_dependencies",
                          "incompatible_dependencies" };
  GPtrArray *arrays[] = { entry->required_dependencies,
                          entry->optional_dependencies,
                          entry->incompatible_dependencies };

  for (guint kind = 0; kind < G_N_ELEMENTS (names); kind++)
    {
      JsonNode *node = json_object_get_member (object, names[kind]);
      if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
        continue;
      for (guint i = 0; i < json_array_get_length (json_node_get_array (node));
           i++)
        {
          JsonNode *item = json_array_get_element (json_node_get_array (node), i);
          if (JSON_NODE_HOLDS_VALUE (item) &&
              json_node_get_value_type (item) == G_TYPE_STRING)
            g_ptr_array_add (arrays[kind], json_node_dup_string (item));
        }
    }
}

static AmlContentIndexEntry *
entry_from_object (JsonObject *object, GError **error)
{
  AmlContentIndexEntry *entry = g_new0 (AmlContentIndexEntry, 1);

  entry->filename = g_strdup (json_object_get_string_member_with_default (
    object, "filename", ""));
  if (*entry->filename == '\0')
    {
      g_set_error_literal (error, AML_CONTENT_INDEX_ERROR,
                           AML_CONTENT_INDEX_ERROR_INVALID,
                           "content metadata has no filename");
      aml_content_index_entry_free (entry);
      return NULL;
    }
  {
    JsonNode *node = json_object_get_member (object, "size");
    if (node != NULL && JSON_NODE_HOLDS_VALUE (node))
      entry->size = (guint64) json_node_get_int (node);
    node = json_object_get_member (object, "mtime_usec");
    if (node != NULL && JSON_NODE_HOLDS_VALUE (node))
      entry->mtime_usec = json_node_get_int (node);
  }
  entry->hash = g_strdup (json_object_get_string_member_with_default (
    object, "hash", ""));
  entry->hash512 = g_strdup (json_object_get_string_member_with_default (
    object, "hash512", ""));
  entry->download_url = g_strdup (json_object_get_string_member_with_default (
    object, "download_url", ""));
  entry->env_client = g_strdup (json_object_get_string_member_with_default (
    object, "env_client", "required"));
  entry->env_server = g_strdup (json_object_get_string_member_with_default (
    object, "env_server", "required"));
  entry->provider = g_strdup (json_object_get_string_member_with_default (
    object, "provider", INDEX_PROVIDER));
  entry->project_id = g_strdup (json_object_get_string_member_with_default (
    object, "project_id", ""));
  entry->project_name = g_strdup (json_object_get_string_member_with_default (
    object, "project_name", ""));
  entry->icon_url = g_strdup (json_object_get_string_member_with_default (
    object, "icon_url", ""));
  entry->metadata_checked = json_object_get_boolean_member_with_default (
    object, "metadata_checked", FALSE);
  entry->local_icon_member = g_strdup (
    json_object_get_string_member_with_default (object, "local_icon_member", ""));
  entry->local_icon_path = g_strdup (
    json_object_get_string_member_with_default (object, "local_icon_path", ""));
  entry->version_id = g_strdup (json_object_get_string_member_with_default (
    object, "version_id", ""));
  entry->version_name = g_strdup (json_object_get_string_member_with_default (
    object, "version_name", ""));
  entry->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  entry->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  entry->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  read_dependencies (object, entry);
  return entry;
}

static AmlContentIndexEntry *
entry_from_json (const char *contents, gsize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) parse_error = NULL;
  JsonNode *root = NULL;

  /* Callers bound the file before they get here, so the length always
     fits the signed one the parser takes. */
  if (!json_parser_load_from_data (parser, contents, (gssize) length,
                                  &parse_error))
    {
      g_set_error (error, AML_CONTENT_INDEX_ERROR,
                   AML_CONTENT_INDEX_ERROR_INVALID, "%s",
                   parse_error->message);
      return NULL;
    }
  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, AML_CONTENT_INDEX_ERROR,
                           AML_CONTENT_INDEX_ERROR_INVALID,
                           "content metadata is not an object");
      return NULL;
    }
  return entry_from_object (json_node_get_object (root), error);
}

static GPtrArray *
load_entries_sync (const char *instance_directory, AmlContentType type,
                  GError **error)
{
  g_autofree char *directory = index_directory (instance_directory, type);
  GDir *dir = g_dir_open (directory, 0, NULL);
  GPtrArray *entries = empty_entries ();

  if (dir == NULL)
    return entries;

  for (const char *name = g_dir_read_name (dir); name != NULL;
       name = g_dir_read_name (dir))
    {
      g_autofree char *path = NULL;
      g_autofree char *contents = NULL;
      g_autoptr (GError) local_error = NULL;
      gsize length = 0;
      AmlContentIndexEntry *entry = NULL;

      if (!g_str_has_suffix (name, ".json"))
        continue;
      path = g_build_filename (directory, name, NULL);
      if (!aml_fs_get_contents_within (path, AML_FS_MAX_CONTENT_INDEX_SIZE,
                                       &contents, &length, &local_error))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          g_dir_close (dir);
          g_ptr_array_unref (entries);
          return NULL;
        }
      entry = entry_from_json (contents, length, &local_error);
      if (entry == NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          g_dir_close (dir);
          g_ptr_array_unref (entries);
          return NULL;
        }
      g_ptr_array_add (entries, entry);
    }
  g_dir_close (dir);
  return entries;
}

typedef struct {
  char *instance_directory;
  AmlContentType type;

  /* Borrowed: this is the task's own data and the scope owns the task. */
  AmlTaskScope *scope;
} LoadArgs;

static void
load_args_free (LoadArgs *args)
{
  g_free (args->instance_directory);
  g_free (args);
}

static void
load_thread (GTask *task G_GNUC_UNUSED, gpointer source G_GNUC_UNUSED,
             gpointer task_data,
             GCancellable *cancellable G_GNUC_UNUSED)
{
  LoadArgs *args = task_data;
  g_autoptr (GError) error = NULL;
  GPtrArray *entries = load_entries_sync (args->instance_directory, args->type,
                                          &error);

  if (entries == NULL)
    aml_task_scope_return_error (args->scope, g_steal_pointer (&error));
  else
    aml_task_scope_return_pointer (args->scope, entries,
                                   (GDestroyNotify) g_ptr_array_unref);
}

AmlTaskScope *
aml_content_index_load_async (const char *instance_directory,
                              AmlContentType type,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  LoadArgs *args = g_new0 (LoadArgs, 1);

  args->instance_directory = g_strdup (instance_directory);
  args->type = type;
  args->scope = scope;
  g_task_set_task_data (task, args, (GDestroyNotify) load_args_free);
  g_task_run_in_thread (task, load_thread);

  return g_steal_pointer (&scope);
}

GPtrArray *
aml_content_index_load_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

GPtrArray *
aml_content_index_load_sync (const char *instance_directory, AmlContentType type,
                             GError **error)
{
  return load_entries_sync (instance_directory, type, error);
}

static void
add_string_array (JsonBuilder *builder, const char *name, GPtrArray *values)
{
  json_builder_set_member_name (builder, name);
  json_builder_begin_array (builder);
  for (guint i = 0; values != NULL && i < values->len; i++)
    json_builder_add_string_value (builder, g_ptr_array_index (values, i));
  json_builder_end_array (builder);
}

static char *
entry_to_json (AmlContentIndexEntry *entry, gsize *length)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
#define ADD_STRING(name, value) \
  G_STMT_START { json_builder_set_member_name (builder, name); \
    json_builder_add_string_value (builder, (value) != NULL ? (value) : ""); } G_STMT_END
  ADD_STRING ("filename", entry->filename);
  json_builder_set_member_name (builder, "size");
  json_builder_add_int_value (builder, (gint64) entry->size);
  json_builder_set_member_name (builder, "mtime_usec");
  json_builder_add_int_value (builder, entry->mtime_usec);
  ADD_STRING ("hash", entry->hash);
  ADD_STRING ("hash512", entry->hash512);
  ADD_STRING ("download_url", entry->download_url);
  ADD_STRING ("env_client", entry->env_client);
  ADD_STRING ("env_server", entry->env_server);
  ADD_STRING ("provider", entry->provider != NULL ? entry->provider : INDEX_PROVIDER);
  ADD_STRING ("project_id", entry->project_id);
  ADD_STRING ("project_name", entry->project_name);
  ADD_STRING ("icon_url", entry->icon_url);
  json_builder_set_member_name (builder, "metadata_checked");
  json_builder_add_boolean_value (builder, entry->metadata_checked);
  ADD_STRING ("local_icon_member", entry->local_icon_member);
  ADD_STRING ("local_icon_path", entry->local_icon_path);
  ADD_STRING ("version_id", entry->version_id);
  ADD_STRING ("version_name", entry->version_name);
#undef ADD_STRING
  add_string_array (builder, "required_dependencies", entry->required_dependencies);
  add_string_array (builder, "optional_dependencies", entry->optional_dependencies);
  add_string_array (builder, "incompatible_dependencies", entry->incompatible_dependencies);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  json_generator_set_pretty (generator, TRUE);
  return json_generator_to_data (generator, length);
}

typedef struct {
  char *path;
  char *directory;
  char *data;
  gsize length;

  /* Borrowed: the operation the caller owns until the callback runs. */
  AmlTaskScope *scope;
} WriteRun;

static void
write_run_free (WriteRun *run)
{
  g_free (run->path);
  g_free (run->directory);
  g_free (run->data);
  g_free (run);
}

static void
write_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  WriteRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (aml_fs_replace_contents_finish (G_FILE (source), result, NULL, &error))
    aml_task_scope_return_boolean (run->scope, TRUE);
  else
    aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
  write_run_free (run);
}

static AmlTaskScope *
write_entry_async (const char *instance_directory, AmlContentType type,
                   AmlContentIndexEntry *entry, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  WriteRun *run = g_new0 (WriteRun, 1);

  run->directory = index_directory (instance_directory, type);
  run->path = aml_content_index_entry_path (instance_directory, type,
                                            entry->filename);
  run->data = entry_to_json (entry, &run->length);
  run->scope = scope;

  g_autoptr (GError) directory_error = NULL;
  if (!aml_fs_make_directory (run->directory, 0755, &directory_error))
    {
      aml_task_scope_return_error_deferred (
        scope,
        g_error_new (AML_CONTENT_INDEX_ERROR,
                     AML_CONTENT_INDEX_ERROR_INVALID,
                     "cannot create index directory \"%s\": %s",
                     run->directory, directory_error->message));
      write_run_free (run);
      return g_steal_pointer (&scope);
    }

  {
    g_autoptr (GFile) file = g_file_new_for_path (run->path);

    aml_fs_replace_contents_async (file, run->data, run->length, NULL, FALSE,
                                   G_FILE_CREATE_REPLACE_DESTINATION,
                                   aml_task_scope_get_cancellable (scope),
                                   write_done, run);
  }

  return g_steal_pointer (&scope);
}

typedef struct {
  char *path;

  /* Borrowed: the operation the caller owns until the callback runs. */
  AmlTaskScope *scope;
} RemoveRun;

static void
remove_run_free (RemoveRun *run)
{
  g_free (run->path);
  g_free (run);
}

static void
remove_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RemoveRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_fs_delete_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
  else
    aml_task_scope_return_boolean (run->scope, TRUE);
  remove_run_free (run);
}

AmlTaskScope *
aml_content_index_update_async (const char *instance_directory,
                                AmlContentType type,
                                AmlContentIndexEntry *entry,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_return_val_if_fail (entry != NULL, NULL);

  return write_entry_async (instance_directory, type, entry, callback,
                            user_data);
}

AmlTaskScope *
aml_content_index_remove_async (const char *instance_directory,
                                AmlContentType type, const char *filename,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  RemoveRun *run = g_new0 (RemoveRun, 1);
  g_autoptr (GFile) file = NULL;

  run->path = aml_content_index_entry_path (instance_directory, type, filename);
  run->scope = scope;
  file = g_file_new_for_path (run->path);
  aml_fs_delete_async (file, G_PRIORITY_DEFAULT,
                       aml_task_scope_get_cancellable (scope), remove_done, run);

  return g_steal_pointer (&scope);
}

gboolean
aml_content_index_modify_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}
