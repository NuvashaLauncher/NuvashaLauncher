/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* File discovery, hashing, and enabled/disabled filename handling for an
 * instance's content folder. This is independent of the page widgets. */

#include "nuvasha-content-page.h"
#include "nuvasha-content-files-private.h"
#include "nuvasha-content-index.h"
#include "nuvasha-download.h"
#include "nuvasha-icons.h"
#include "nuvasha-task-private.h"
#include "nuvasha-fs.h"

#include <archive.h>
#include <archive_entry.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * The files of the instance folder
 * ---------------------------------------------------------- */

void
nuvasha_content_file_free (NuvashaContentFile *file)
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
  g_free (file->local_icon_member);
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
nuvasha_content_file_refresh_search_key (NuvashaContentFile *file)
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
nuvasha_content_folder_path (const char *instance_directory, NuvashaContentType type)
{
  g_return_val_if_fail (instance_directory != NULL, NULL);

  return g_build_filename (instance_directory,
                           nuvasha_content_type_folder (type), NULL);
}

char *
nuvasha_content_disabled_name (const char *filename, gboolean enabled)
{
  const char *suffix = nuvasha_CONTENT_DISABLED_SUFFIX;
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
nuvasha_content_set_disabled (const char *path, gboolean disabled, GError **error)
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
  name = nuvasha_content_disabled_name (base, !disabled);
  target = g_build_filename (directory, name, NULL);

  if (g_strcmp0 (path, target) == 0)
    return g_steal_pointer (&target);

  from = g_file_new_for_path (path);
  to = g_file_new_for_path (target);

  /* A rename of a local file is one system call: it waits for nothing
   * and moves no byte of the file itself. */
  if (!nuvasha_fs_move_file (from, to, G_FILE_COPY_NONE, NULL, error))
    return NULL;

  return g_steal_pointer (&target);
}

gboolean
nuvasha_content_remove_file (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return nuvasha_fs_unlink (path, error);
}

gboolean
nuvasha_content_ensure_folder (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return nuvasha_fs_make_directory (path, 0755, error);
}

void
nuvasha_content_delete_async (GFile *file, int io_priority,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  nuvasha_fs_delete_async (file, io_priority, cancellable, callback, user_data);
}

gboolean
nuvasha_content_delete_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return nuvasha_fs_delete_finish (file, result, error);
}

/* TRUE when @name is a file of this content, suffix or not: the loaders
 * and the game only pick up the ones that do not carry the suffix. */
static gboolean
name_is_content (const char *name, const char *extension)
{
  g_autofree char *plain = nuvasha_content_disabled_name (name, TRUE);

  return g_str_has_suffix (plain, extension);
}

/* The scan: what the folder holds and the hash of each file, which is
 * what Modrinth is then asked about. */
typedef struct
{
  /* Borrowed: the caller's handle keeps the operation alive, and every
     path that frees this run has already answered it. */
  NuvashaTaskScope *scope;
  GFile *folder;
  GFileEnumerator *enumerator; /* while the batches are read */
  char *instance_directory;
  NuvashaContentType type;
  char *extension;
  GPtrArray *files; /* NuvashaContentFile* */
  GPtrArray *index_entries; /* NuvashaContentIndexEntry* */
  guint index;
  guint hash_pending;
  gboolean hash_cancelled;
  guint local_pending;
  gboolean local_cancelled;

  /* The index load that has to finish before the scan can move on. */
  NuvashaTaskScope *index_operation;
} ScanRun;

static void scan_hash_next (ScanRun *run);
static void batch_read_next (ScanRun *run);
static int compare_files (gconstpointer a, gconstpointer b);
static void scan_free (ScanRun *run);

typedef struct {
  ScanRun *run;
  NuvashaContentFile *file;
} HashInput;

static void
scan_finish (ScanRun *run)
{
  g_ptr_array_sort (run->files, compare_files);
  nuvasha_task_scope_return_pointer (run->scope, g_steal_pointer (&run->files),
                                 (GDestroyNotify) g_ptr_array_unref);
  scan_free (run);
}

typedef struct
{
  NuvashaTaskScope *scope;
  GPtrArray *files;
  NuvashaContentType type;
  gint pending;
} MetadataRun;

typedef struct
{
  MetadataRun *run;
  NuvashaContentFile *file;
} MetadataWork;

static GThreadPool *metadata_pool;
static void metadata_run_free (MetadataRun *run);

static void
metadata_run_complete (MetadataRun *run)
{
  if (!g_atomic_int_dec_and_test (&run->pending))
    return;

  if (g_cancellable_is_cancelled (nuvasha_task_scope_get_cancellable (run->scope)))
    nuvasha_task_scope_return_error (
      run->scope,
      g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
  else
    nuvasha_task_scope_return_boolean (run->scope, TRUE);

  metadata_run_free (run);
}

static void
metadata_work (gpointer data, gpointer user_data G_GNUC_UNUSED)
{
  MetadataWork *work = data;
  MetadataRun *run = work->run;

  if (!g_cancellable_is_cancelled (nuvasha_task_scope_get_cancellable (run->scope)))
    {
      NuvashaContentFile *file = work->file;
      gboolean needs_metadata = !file->metadata_checked;

      if (needs_metadata)
        nuvasha_content_apply_local_metadata (file, run->type);
      file->metadata_checked = TRUE;
      file->metadata_dirty = needs_metadata;
      nuvasha_content_file_refresh_search_key (file);
    }

  metadata_run_complete (run);
  g_free (work);
}

static void
metadata_run_free (MetadataRun *run)
{
  g_ptr_array_unref (run->files);
  nuvasha_task_scope_unref (run->scope);
  g_free (run);
}

static void
local_metadata_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  MetadataRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    nuvasha_task_scope_return_error (run->scope, g_steal_pointer (&error));
  else
    nuvasha_task_scope_return_boolean (run->scope, TRUE);

  metadata_run_free (run);
}

NuvashaTaskScope *
nuvasha_content_metadata_async (GPtrArray *files, NuvashaContentType type,
                            GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  MetadataRun *run = g_new0 (MetadataRun, 1);
  GTask *task = NULL;

  run->scope = nuvasha_task_scope_ref (scope);
  run->files = g_ptr_array_ref (files);
  run->type = type;

  if (metadata_pool == NULL)
    metadata_pool = g_thread_pool_new (metadata_work, NULL, 4, FALSE, NULL);

  if (files->len == 0)
    {
      task = g_task_new (NULL, nuvasha_task_scope_get_cancellable (scope),
                         local_metadata_done, run);
      nuvasha_task_scope_attach_task (task);
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return g_steal_pointer (&scope);
    }

  g_atomic_int_set (&run->pending, files->len);
  for (guint i = 0; i < files->len; i++)
    {
      MetadataWork *work = g_new (MetadataWork, 1);
      work->run = run;
      work->file = g_ptr_array_index (files, i);
      g_thread_pool_push (metadata_pool, work, NULL);
    }

  return g_steal_pointer (&scope);
}

gboolean
nuvasha_content_metadata_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
scan_free (ScanRun *run)
{
  g_clear_object (&run->enumerator);
  g_clear_object (&run->folder);
  if (run->index_operation != NULL)
    nuvasha_task_scope_unref (run->index_operation);
  g_clear_pointer (&run->files, g_ptr_array_unref);
  g_clear_pointer (&run->index_entries, g_ptr_array_unref);
  g_free (run->instance_directory);
  g_free (run->extension);
  g_free (run);
}

static NuvashaContentIndexEntry *
scan_index_entry (ScanRun *run, NuvashaContentFile *file)
{
  g_autofree char *name = g_path_get_basename (file->path);

  for (guint i = 0; run->index_entries != NULL &&
                     i < run->index_entries->len; i++)
    {
      NuvashaContentIndexEntry *entry = g_ptr_array_index (run->index_entries, i);
      if (g_strcmp0 (entry->filename, name) == 0)
        {
          if (entry->size == file->size &&
              entry->mtime_usec == file->mtime_usec &&
              entry->hash != NULL && *entry->hash != '\0')
            return entry;
          return NULL;
        }
    }
  return NULL;
}

static void
apply_index_entry (ScanRun *run, NuvashaContentFile *file)
{
  NuvashaContentIndexEntry *entry = scan_index_entry (run, file);

  if (entry == NULL)
    return;
  if (entry->hash != NULL && *entry->hash != '\0')
    file->sha1 = g_strdup (entry->hash);
  if (entry->project_id != NULL && *entry->project_id != '\0')
    file->project_id = g_strdup (entry->project_id);
  if (entry->hash512 != NULL && *entry->hash512 != '\0')
    file->sha512 = g_strdup (entry->hash512);
  if (entry->download_url != NULL && *entry->download_url != '\0')
    file->download_url = g_strdup (entry->download_url);
  if (!file->local_metadata && entry->project_name != NULL &&
      *entry->project_name != '\0' &&
      g_strcmp0 (entry->project_name, entry->project_id) != 0)
    {
      g_free (file->project_name);
      file->project_name = g_strdup (entry->project_name);
    }
  if (entry->icon_url != NULL && *entry->icon_url != '\0')
    file->icon_url = g_strdup (entry->icon_url);
  file->metadata_checked = entry->metadata_checked;
  if (entry->local_icon_member != NULL && *entry->local_icon_member != '\0')
    file->local_icon_member = g_strdup (entry->local_icon_member);
  if (entry->local_icon_path != NULL && *entry->local_icon_path != '\0')
    file->local_icon_path = g_strdup (entry->local_icon_path);
  if (entry->version_id != NULL && *entry->version_id != '\0')
    file->version_id = g_strdup (entry->version_id);
  if (entry->version_name != NULL && *entry->version_name != '\0')
    file->version_name = g_strdup (entry->version_name);
  nuvasha_content_merge_dependencies (&file->required_dependencies,
                      entry->required_dependencies);
  nuvasha_content_merge_dependencies (&file->optional_dependencies,
                      entry->optional_dependencies);
  nuvasha_content_merge_dependencies (&file->incompatible_dependencies,
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

  if (g_cancellable_is_cancelled (nuvasha_task_scope_get_cancellable (run->scope)))
    {
      scan_free (run);
      return;
    }

  /* No list and no error is how an enumeration says it has read the
   * whole folder. */
  if (children == NULL && error != NULL)
    {
      nuvasha_task_scope_return_error (run->scope, g_steal_pointer (&error));
      scan_free (run);
      return;
    }

  for (GList *item = children; item != NULL; item = item->next)
    {
      GFileInfo *info = item->data;
      /* Borrowed from @info, which is dropped with the list. */
      const char *name = g_file_info_get_name (info);
      g_autoptr (GFile) child = NULL;
      NuvashaContentFile *file = NULL;

      if (name == NULL ||
          g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
        continue;

      if (!name_is_content (name, run->extension))
        continue;

      child = g_file_get_child (run->folder, name);
      file = g_new0 (NuvashaContentFile, 1);
      file->path = g_file_get_path (child);
      file->filename = nuvasha_content_disabled_name (name, TRUE);
      file->size = g_file_info_get_size (info);
      file->mtime_usec = g_file_info_get_attribute_uint32 (
        info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
      file->enabled = g_strcmp0 (name, file->filename) == 0;
      apply_index_entry (run, file);
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
                                       nuvasha_task_scope_get_cancellable (run->scope),
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
       nuvasha_task_scope_return_error (run->scope, g_steal_pointer (&error));
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
    nuvasha_content_index_load_finish (result, &error);

  if (run->index_operation != NULL)
    {
      nuvasha_task_scope_unref (run->index_operation);
      run->index_operation = NULL;
    }

  /* A missing or damaged index is deliberately not fatal: the existing
   * hash lookup repairs what it can, and the folder remains usable offline. */
  if (entries == NULL)
    {
      g_clear_error (&error);
      entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify) nuvasha_content_index_entry_free);
    }
  run->index_entries = g_steal_pointer (&entries);

  g_file_enumerate_children_async (
    run->folder,
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
     nuvasha_task_scope_get_cancellable (run->scope), scan_children_ready, run);
}

static void
scan_hash_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  HashInput *input = user_data;
  ScanRun *run = input->run;
  NuvashaContentFile *file = input->file;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = NULL;

  digest = nuvasha_download_file_hash_finish (result, &error);

   if (g_cancellable_is_cancelled (nuvasha_task_scope_get_cancellable (run->scope)))
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
           nuvasha_task_scope_return_error (
             run->scope,
             g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                  "cancelled"));
          scan_free (run);
        }
      else
        scan_finish (run);
    }
}

static void
scan_hash_next (ScanRun *run)
{
  run->hash_pending = 0;
  for (guint i = 0; i < run->files->len; i++)
    if (((NuvashaContentFile *) g_ptr_array_index (run->files, i))->sha1 == NULL)
      run->hash_pending++;

  if (run->hash_pending == 0)
    {
      scan_finish (run);
      return;
    }
  for (guint i = 0; i < run->files->len; i++)
    {
      NuvashaContentFile *file = g_ptr_array_index (run->files, i);

      if (file->sha1 != NULL)
        continue;

      g_autoptr (GFile) handle = g_file_new_for_path (file->path);
      HashInput *input = g_new (HashInput, 1);
      input->run = run;
      input->file = file;
      nuvasha_task_scope_release_when_done (
          nuvasha_download_file_hash_async (handle, G_CHECKSUM_SHA1,
                                        scan_hash_ready, input),
          nuvasha_task_scope_get_cancellable (run->scope));
    }
}

static int
compare_files (gconstpointer a, gconstpointer b)
{
  const NuvashaContentFile *left = *(const NuvashaContentFile *const *) a;
  const NuvashaContentFile *right = *(const NuvashaContentFile *const *) b;

  return g_utf8_collate (left->filename, right->filename);
}

NuvashaTaskScope *
nuvasha_content_scan_async (const char *instance_directory, NuvashaContentType type,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  ScanRun *run = NULL;
  g_autofree char *folder = NULL;

  g_return_val_if_fail (instance_directory != NULL, NULL);

  folder = nuvasha_content_folder_path (instance_directory, type);
  run = g_new0 (ScanRun, 1);
  run->scope = scope;
  nuvasha_task_scope_set_check_cancellable (run->scope, FALSE);
  run->folder = g_file_new_for_path (folder);
  run->instance_directory = g_strdup (instance_directory);
  run->type = type;
  run->extension = g_strdup (nuvasha_content_type_extension (type));
  run->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvasha_content_file_free);

  /* An instance that never installed anything of this kind has no such
   * folder, which reads the same as an empty one. */
  if (!g_file_query_exists (run->folder, NULL))
    {
      nuvasha_task_scope_return_pointer_deferred (scope,
                                              g_steal_pointer (&run->files),
                                              (GDestroyNotify) g_ptr_array_unref);
      scan_free (run);
      return g_steal_pointer (&scope);
    }

  run->index_operation = nuvasha_content_index_load_async (instance_directory, type,
                                                       scan_index_ready, run);

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvasha_content_scan_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
