/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-index.h"

#include "aml-content-page.h"

#include <json-glib/json-glib.h>

#define INDEX_DIRECTORY ".index"
#define INDEX_FILE "index.json"
#define INDEX_PROVIDER "Modrinth"

static char *
index_path (const char *instance_directory, AmlContentType type)
{
  g_autofree char *folder = aml_content_folder_path (instance_directory, type);

  return g_build_filename (folder, INDEX_DIRECTORY, INDEX_FILE, NULL);
}

static char *
index_directory (const char *instance_directory, AmlContentType type)
{
  g_autofree char *folder = aml_content_folder_path (instance_directory, type);

  return g_build_filename (folder, INDEX_DIRECTORY, NULL);
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

static GPtrArray *
entries_from_json (const char *contents, gsize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) parse_error = NULL;
  GPtrArray *entries = empty_entries ();
  JsonNode *root = NULL;
  JsonNode *items = NULL;

  if (!json_parser_load_from_data (parser, contents, length, &parse_error))
    {
      g_set_error (error, AML_CONTENT_INDEX_ERROR,
                   AML_CONTENT_INDEX_ERROR_INVALID, "%s",
                   parse_error->message);
      g_ptr_array_unref (entries);
      return NULL;
    }

  root = json_parser_get_root (parser);
  items = root != NULL && JSON_NODE_HOLDS_OBJECT (root)
            ? json_object_get_member (json_node_get_object (root), "entries")
            : NULL;
  if (items == NULL || !JSON_NODE_HOLDS_ARRAY (items))
    {
      g_set_error_literal (error, AML_CONTENT_INDEX_ERROR,
                           AML_CONTENT_INDEX_ERROR_INVALID,
                           "the content index has no entries");
      g_ptr_array_unref (entries);
      return NULL;
    }

  for (guint i = 0; i < json_array_get_length (json_node_get_array (items)); i++)
    {
      JsonNode *item = json_array_get_element (json_node_get_array (items), i);
      JsonObject *object = JSON_NODE_HOLDS_OBJECT (item)
                             ? json_node_get_object (item) : NULL;
      AmlContentIndexEntry *entry = NULL;
      JsonNode *dependencies = NULL;

      if (object == NULL)
        continue;
      entry = g_new0 (AmlContentIndexEntry, 1);
      entry->filename = g_strdup (json_object_get_string_member_with_default (
        object, "filename", ""));
      entry->hash = g_strdup (json_object_get_string_member_with_default (
        object, "hash", ""));
      entry->hash512 = g_strdup (
        json_object_get_string_member_with_default (object, "hash512", ""));
      entry->download_url = g_strdup (
        json_object_get_string_member_with_default (object, "download_url", ""));
      entry->env_client = g_strdup (
        json_object_get_string_member_with_default (object, "env_client",
                                                   "required"));
      entry->env_server = g_strdup (
        json_object_get_string_member_with_default (object, "env_server",
                                                   "required"));
      entry->provider = g_strdup (json_object_get_string_member_with_default (
        object, "provider", INDEX_PROVIDER));
      entry->project_id = g_strdup (json_object_get_string_member_with_default (
        object, "project_id", ""));
      entry->project_name = g_strdup (json_object_get_string_member_with_default (
        object, "project_name", ""));
      entry->icon_url = g_strdup (json_object_get_string_member_with_default (
        object, "icon_url", ""));
      entry->version_id = g_strdup (json_object_get_string_member_with_default (
        object, "version_id", ""));
      entry->version_name = g_strdup (json_object_get_string_member_with_default (
        object, "version_name", ""));
      entry->required_dependencies = g_ptr_array_new_with_free_func (g_free);
      entry->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
      entry->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);

      const char *names[] = { "required_dependencies", "optional_dependencies",
                              "incompatible_dependencies" };
      GPtrArray **arrays[] = { &entry->required_dependencies,
                               &entry->optional_dependencies,
                               &entry->incompatible_dependencies };
      for (guint kind = 0; kind < G_N_ELEMENTS (names); kind++)
        {
          dependencies = json_object_get_member (object, names[kind]);
          if (dependencies == NULL || !JSON_NODE_HOLDS_ARRAY (dependencies))
            continue;
          for (guint j = 0; j < json_array_get_length (
                 json_node_get_array (dependencies)); j++)
            {
              JsonNode *dependency = json_array_get_element (
                json_node_get_array (dependencies), j);
              if (JSON_NODE_HOLDS_VALUE (dependency) &&
                  json_node_get_value_type (dependency) == G_TYPE_STRING)
                g_ptr_array_add (*arrays[kind], json_node_dup_string (dependency));
            }
        }
      g_ptr_array_add (entries, entry);
    }

  return entries;
}

typedef struct {
  GTask *task;
  char *path;
} LoadRun;

static void
load_run_free (LoadRun *run)
{
  g_clear_object (&run->task);
  g_free (run->path);
  g_free (run);
}

static void
index_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;
  GPtrArray *entries = NULL;

  if (!g_file_load_contents_finish (G_FILE (source), result, &contents,
                                    &length, NULL, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_task_return_pointer (run->task, empty_entries (),
                               (GDestroyNotify) g_ptr_array_unref);
      else
        g_task_return_error (run->task, g_steal_pointer (&error));
      load_run_free (run);
      return;
    }

  entries = entries_from_json (contents, length, &error);
  if (entries == NULL)
    g_task_return_error (run->task, g_steal_pointer (&error));
  else
    g_task_return_pointer (run->task, entries,
                           (GDestroyNotify) g_ptr_array_unref);
  load_run_free (run);
}

void
aml_content_index_load_async (const char *instance_directory,
                              AmlContentType type, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  LoadRun *run = g_new0 (LoadRun, 1);
  g_autoptr (GFile) file = NULL;

  run->task = g_task_new (NULL, cancellable, callback, user_data);
  run->path = index_path (instance_directory, type);
  file = g_file_new_for_path (run->path);
  g_file_load_contents_async (file, cancellable, index_loaded, run);
}

GPtrArray *
aml_content_index_load_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
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
entries_to_json (GPtrArray *entries, gsize *length)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "version");
  json_builder_add_int_value (builder, 1);
  json_builder_set_member_name (builder, "entries");
  json_builder_begin_array (builder);
  for (guint i = 0; i < entries->len; i++)
    {
      AmlContentIndexEntry *entry = g_ptr_array_index (entries, i);
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "filename");
      json_builder_add_string_value (builder, entry->filename);
      json_builder_set_member_name (builder, "hash");
      json_builder_add_string_value (builder, entry->hash);
      json_builder_set_member_name (builder, "hash512");
      json_builder_add_string_value (builder, entry->hash512);
      json_builder_set_member_name (builder, "download_url");
      json_builder_add_string_value (builder, entry->download_url);
      json_builder_set_member_name (builder, "env_client");
      json_builder_add_string_value (builder, entry->env_client);
      json_builder_set_member_name (builder, "env_server");
      json_builder_add_string_value (builder, entry->env_server);
      json_builder_set_member_name (builder, "provider");
      json_builder_add_string_value (builder, entry->provider != NULL
                                               ? entry->provider : INDEX_PROVIDER);
      json_builder_set_member_name (builder, "project_id");
      json_builder_add_string_value (builder, entry->project_id);
      json_builder_set_member_name (builder, "project_name");
      json_builder_add_string_value (builder, entry->project_name);
      json_builder_set_member_name (builder, "icon_url");
      json_builder_add_string_value (builder, entry->icon_url != NULL
                                               ? entry->icon_url : "");
      json_builder_set_member_name (builder, "version_id");
      json_builder_add_string_value (builder, entry->version_id);
      json_builder_set_member_name (builder, "version_name");
      json_builder_add_string_value (builder, entry->version_name);
      add_string_array (builder, "required_dependencies", entry->required_dependencies);
      add_string_array (builder, "optional_dependencies", entry->optional_dependencies);
      add_string_array (builder, "incompatible_dependencies",
                        entry->incompatible_dependencies);
      json_builder_end_object (builder);
    }
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  json_generator_set_pretty (generator, TRUE);
  return json_generator_to_data (generator, length);
}

typedef struct {
  GTask *task;
  char *path;
  char *directory;
  char *data;
  gsize length;
} WriteRun;

static void
write_run_free (WriteRun *run)
{
  g_clear_object (&run->task);
  g_free (run->path);
  g_free (run->directory);
  g_free (run->data);
  g_free (run);
}

static void
index_write_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  WriteRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    g_task_return_error (run->task, g_steal_pointer (&error));
  else
    g_task_return_boolean (run->task, TRUE);
  write_run_free (run);
}

static void
write_entries_async (const char *instance_directory, AmlContentType type,
                     GPtrArray *entries, GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data)
{
  WriteRun *run = g_new0 (WriteRun, 1);
  g_autoptr (GFile) file = NULL;

  run->task = g_task_new (NULL, cancellable, callback, user_data);
  run->directory = index_directory (instance_directory, type);
  run->path = index_path (instance_directory, type);
  run->data = entries_to_json (entries, &run->length);
  file = g_file_new_for_path (run->path);

  if (g_mkdir_with_parents (run->directory, 0755) != 0 && errno != EEXIST)
    {
      g_task_return_new_error (run->task, AML_CONTENT_INDEX_ERROR,
                               AML_CONTENT_INDEX_ERROR_INVALID,
                               "cannot create index directory \"%s\": %s",
                               run->directory, g_strerror (errno));
      write_run_free (run);
      return;
    }

  g_file_replace_contents_async (file, run->data, run->length, NULL, FALSE,
                                 G_FILE_CREATE_REPLACE_DESTINATION, cancellable,
                                 index_write_done, run);
}

typedef struct {
  char *instance_directory;
  AmlContentType type;
  AmlContentIndexEntry *entry;
  char *remove_filename;
  GCancellable *cancellable;
  GAsyncReadyCallback callback;
  gpointer user_data;
} ModifyRun;

/* Index updates are read/modify/write operations. Keeping them in one queue
 * prevents two metadata repairs from loading the same old index and letting
 * the later write discard the entry added by the first one. */
static GQueue pending_modifications = G_QUEUE_INIT;
static gboolean modification_active;

static void modify_start_next (void);
static void modify_written (GObject *source, GAsyncResult *result,
                            gpointer user_data);

static void
modify_free (ModifyRun *run)
{
  g_free (run->instance_directory);
  aml_content_index_entry_free (run->entry);
  g_free (run->remove_filename);
  g_clear_object (&run->cancellable);
  g_free (run);
}

static AmlContentIndexEntry *
entry_copy (AmlContentIndexEntry *source)
{
  AmlContentIndexEntry *entry = g_new0 (AmlContentIndexEntry, 1);
  entry->filename = g_strdup (source->filename);
  entry->hash = g_strdup (source->hash);
  entry->hash512 = g_strdup (source->hash512);
  entry->download_url = g_strdup (source->download_url);
  entry->env_client = g_strdup (source->env_client);
  entry->env_server = g_strdup (source->env_server);
  entry->provider = g_strdup (source->provider);
  entry->project_id = g_strdup (source->project_id);
  entry->project_name = g_strdup (source->project_name);
  entry->icon_url = g_strdup (source->icon_url);
  entry->version_id = g_strdup (source->version_id);
  entry->version_name = g_strdup (source->version_name);
  entry->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  entry->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  entry->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *arrays[] = { source->required_dependencies,
                          source->optional_dependencies,
                          source->incompatible_dependencies };
  GPtrArray **copies[] = { &entry->required_dependencies,
                           &entry->optional_dependencies,
                           &entry->incompatible_dependencies };
  for (guint kind = 0; kind < G_N_ELEMENTS (arrays); kind++)
    for (guint i = 0; arrays[kind] != NULL && i < arrays[kind]->len; i++)
      g_ptr_array_add (*copies[kind],
                       g_strdup (g_ptr_array_index (arrays[kind], i)));
  return entry;
}

static void
modify_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  ModifyRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) entries =
    aml_content_index_load_finish (result, &error);

  if (entries == NULL)
    {
      /* A missing index is already returned as an empty array by
       * index_loaded(). Any other failure must leave the existing file
       * untouched instead of erasing all of its metadata. */
      if (run->callback != NULL)
        {
          g_autoptr (GTask) task =
            g_task_new (NULL, run->cancellable, run->callback, run->user_data);
          g_task_return_error (task, g_steal_pointer (&error));
        }
      else
        {
          g_warning ("could not read content index for \"%s\": %s",
                     run->instance_directory,
                     error != NULL ? error->message : "unknown error");
        }
      modify_free (run);
      modification_active = FALSE;
      modify_start_next ();
      return;
    }
  for (gint i = (gint) entries->len - 1; i >= 0; i--)
    {
      AmlContentIndexEntry *old = g_ptr_array_index (entries, i);

      if (run->entry != NULL &&
          g_strcmp0 (old->filename, run->entry->filename) == 0)
        {
          /* Repair updates commonly know the project name and icon but not
           * the hashes or URL recorded when the file was installed. Keep
           * those export-critical fields instead of erasing them. */
          if (run->entry->hash512 == NULL || *run->entry->hash512 == '\0')
            {
              g_clear_pointer (&run->entry->hash512, g_free);
              run->entry->hash512 = g_strdup (old->hash512);
            }
          if (run->entry->download_url == NULL ||
              *run->entry->download_url == '\0')
            {
              g_clear_pointer (&run->entry->download_url, g_free);
              run->entry->download_url = g_strdup (old->download_url);
            }
          if (run->entry->env_client == NULL || *run->entry->env_client == '\0')
            {
              g_clear_pointer (&run->entry->env_client, g_free);
              run->entry->env_client = g_strdup (old->env_client);
            }
          if (run->entry->env_server == NULL || *run->entry->env_server == '\0')
            {
              g_clear_pointer (&run->entry->env_server, g_free);
              run->entry->env_server = g_strdup (old->env_server);
            }
        }

      if ((run->entry != NULL && g_strcmp0 (old->filename,
                                             run->entry->filename) == 0) ||
          (run->remove_filename != NULL &&
           g_strcmp0 (old->filename, run->remove_filename) == 0))
        g_ptr_array_remove_index (entries, i);
    }
  if (run->entry != NULL)
    g_ptr_array_add (entries, entry_copy (run->entry));

  /* write_entries_async owns the completion callback; keep the task's
   * callback and user data, then release this modifier after it starts. */
  write_entries_async (run->instance_directory, run->type, entries,
                       run->cancellable, modify_written, run);
  g_clear_pointer (&entries, g_ptr_array_unref);
}

static void
modify_written (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ModifyRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (run->callback != NULL)
    run->callback (source, result, run->user_data);
  else if (!g_task_propagate_boolean (G_TASK (result), &error))
    g_warning ("could not write content index for \"%s\": %s",
               run->instance_directory, error->message);

  modify_free (run);
  modification_active = FALSE;
  modify_start_next ();
}

gboolean
aml_content_index_modify_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
modify_start_next (void)
{
  ModifyRun *run = NULL;

  if (modification_active || g_queue_is_empty (&pending_modifications))
    return;

  run = g_queue_pop_head (&pending_modifications);
  modification_active = TRUE;
  aml_content_index_load_async (run->instance_directory, run->type,
                                run->cancellable, modify_loaded, run);
}

static void
modify_async (const char *instance_directory, AmlContentType type,
              AmlContentIndexEntry *entry, const char *remove_filename,
              GCancellable *cancellable, GAsyncReadyCallback callback,
              gpointer user_data)
{
  ModifyRun *run = g_new0 (ModifyRun, 1);
  run->instance_directory = g_strdup (instance_directory);
  run->type = type;
  run->entry = entry != NULL ? entry_copy (entry) : NULL;
  run->remove_filename = g_strdup (remove_filename);
  run->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  run->callback = callback;
  run->user_data = user_data;
  g_queue_push_tail (&pending_modifications, run);
  modify_start_next ();
}

void
aml_content_index_update_async (const char *instance_directory,
                                AmlContentType type,
                                AmlContentIndexEntry *entry,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail (entry != NULL);
  modify_async (instance_directory, type, entry, NULL, cancellable, callback,
                user_data);
}

void
aml_content_index_remove_async (const char *instance_directory,
                                AmlContentType type, const char *filename,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail (filename != NULL);
  modify_async (instance_directory, type, NULL, filename, cancellable, callback,
                user_data);
}
