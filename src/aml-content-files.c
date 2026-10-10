/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* File discovery, hashing, and enabled/disabled filename handling for an
 * instance's content folder. This is independent of the page widgets. */

#include "aml-content-page.h"
#include "aml-content-index.h"
#include "aml-download.h"

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
  g_free (file->project_name);
  g_free (file->icon_url);
  g_free (file->version_id);
  g_free (file->version_name);
  g_free (file->version_number);
  g_clear_pointer (&file->required_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->optional_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->incompatible_dependencies, g_ptr_array_unref);
  g_free (file);
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
  if (!g_file_move (from, to, G_FILE_COPY_NONE, NULL, NULL, NULL, error))
    return NULL;

  return g_steal_pointer (&target);
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
  GTask *task;
  GFile *folder;
  GFileEnumerator *enumerator; /* while the batches are read */
  char *instance_directory;
  AmlContentType type;
  char *extension;
  GPtrArray *files; /* AmlContentFile* */
  GPtrArray *index_entries; /* AmlContentIndexEntry* */
  guint index;
} ScanRun;

static void scan_hash_next (ScanRun *run);
static void batch_read_next (ScanRun *run);
static int compare_files (gconstpointer a, gconstpointer b);

static void
scan_free (ScanRun *run)
{
  g_clear_object (&run->enumerator);
  g_clear_object (&run->folder);
  g_clear_object (&run->task);
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
  file->project_id = g_strdup (entry->project_id);
  file->sha512 = g_strdup (entry->hash512);
  file->download_url = g_strdup (entry->download_url);
  file->project_name = g_strdup (entry->project_name);
  file->icon_url = g_strdup (entry->icon_url);
  file->version_id = g_strdup (entry->version_id);
  file->version_name = g_strdup (entry->version_name);
  file->required_dependencies = g_ptr_array_ref (entry->required_dependencies);
  file->optional_dependencies = g_ptr_array_ref (entry->optional_dependencies);
  file->incompatible_dependencies = g_ptr_array_ref (entry->incompatible_dependencies);
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

  if (g_task_return_error_if_cancelled (run->task))
    {
      scan_free (run);
      return;
    }

  /* No list and no error is how an enumeration says it has read the
   * whole folder. */
  if (children == NULL && error != NULL)
    {
      g_task_return_error (run->task, g_steal_pointer (&error));
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
                                      g_task_get_cancellable (run->task),
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
      g_task_return_error (run->task, g_steal_pointer (&error));
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
    g_task_get_cancellable (run->task), scan_children_ready, run);
}

static void
scan_hash_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = NULL;
  AmlContentFile *file = g_ptr_array_index (run->files, run->index);

  digest = aml_download_file_hash_finish (result, &error);

  if (g_task_return_error_if_cancelled (run->task))
    {
      scan_free (run);
      return;
    }

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

  run->index++;
  scan_hash_next (run);
}

static void
scan_hash_next (ScanRun *run)
{
  AmlContentFile *file = NULL;
  g_autoptr (GFile) handle = NULL;

  if (run->index >= run->files->len)
    {
      g_ptr_array_sort (run->files, compare_files);
      g_task_return_pointer (run->task, g_steal_pointer (&run->files),
                             (GDestroyNotify) g_ptr_array_unref);
      scan_free (run);
      return;
    }

  file = g_ptr_array_index (run->files, run->index);
  handle = g_file_new_for_path (file->path);
  aml_download_file_hash_async (handle, G_CHECKSUM_SHA1,
                                g_task_get_cancellable (run->task),
                                scan_hash_ready, run);
}

static int
compare_files (gconstpointer a, gconstpointer b)
{
  const AmlContentFile *left = *(const AmlContentFile *const *) a;
  const AmlContentFile *right = *(const AmlContentFile *const *) b;

  return g_utf8_collate (left->filename, right->filename);
}

void
aml_content_scan_async (const char *instance_directory, AmlContentType type,
                        GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  ScanRun *run = NULL;
  g_autofree char *folder = NULL;

  g_return_if_fail (instance_directory != NULL);

  folder = aml_content_folder_path (instance_directory, type);
  run = g_new0 (ScanRun, 1);
  run->task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_check_cancellable (run->task, FALSE);
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
      g_task_return_pointer (run->task, g_steal_pointer (&run->files),
                             (GDestroyNotify) g_ptr_array_unref);
      scan_free (run);
      return;
    }

  aml_content_index_load_async (instance_directory, type, cancellable,
                                scan_index_ready, run);
}

GPtrArray *
aml_content_scan_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

