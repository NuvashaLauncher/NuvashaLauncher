/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "aml-fs.h"
#include "aml-task-private.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

gboolean
aml_fs_replace_contents (const char *path, const void *contents,
                         gssize length, int mode, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!g_file_set_contents_full (path, contents, length,
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_DURABLE,
                                 mode, &local_error))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

gboolean
aml_fs_make_private_directory (const char *path, GError **error)
{
  if (!aml_fs_make_directory (path, 0700, error))
    return FALSE;
  if (g_chmod (path, 0700) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot secure \"%s\": %s", path, g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

gboolean
aml_fs_make_directory (const char *path, int mode, GError **error)
{
  if (g_mkdir_with_parents (path, mode) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot create \"%s\": %s", path, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

gboolean
aml_fs_file_size_within (const char *path, goffset maximum, GError **error)
{
  GStatBuf info;

  if (g_stat (path, &info) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot inspect \"%s\": %s", path, g_strerror (errno));
      return FALSE;
    }

  if (!S_ISREG (info.st_mode) || info.st_size > maximum)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "file \"%s\" exceeds the permitted size", path);
      return FALSE;
    }
  return TRUE;
}

gboolean
aml_fs_unlink (const char *path, GError **error)
{
  if (g_unlink (path) != 0 && errno != ENOENT)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot remove \"%s\": %s", path, g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

gboolean
aml_fs_rename (const char *source, const char *destination, GError **error)
{
  if (g_rename (source, destination) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot rename \"%s\" to \"%s\": %s", source,
                   destination, g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

int
aml_fs_open_temporary_file (const char *directory, const char *prefix,
                            char **path, GError **error)
{
  g_autofree char *template =
    g_build_filename (directory, prefix != NULL ? prefix : ".aml-XXXXXX",
                      NULL);
  int fd = g_mkstemp (template);

  if (fd < 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot create temporary file in \"%s\": %s", directory,
                   g_strerror (errno));
      return -1;
    }

  if (path != NULL)
    *path = g_steal_pointer (&template);
  return fd;
}

char *
aml_fs_make_temporary_directory (const char *parent, const char *prefix,
                                 GError **error)
{
  g_autofree char *template =
    g_build_filename (parent, prefix != NULL ? prefix : ".aml-XXXXXX", NULL);
  char *result = g_mkdtemp (template);

  if (result == NULL)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot create temporary directory in \"%s\": %s",
                   parent, g_strerror (errno));
      return NULL;
    }
  if (g_chmod (result, 0700) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot secure temporary directory \"%s\": %s", result,
                   g_strerror (errno));
      g_rmdir (result);
      return NULL;
    }
  return g_steal_pointer (&template);
}

gboolean
aml_fs_move_file (GFile *source, GFile *destination, GFileCopyFlags flags,
                  GCancellable *cancellable, GError **error)
{
  return g_file_move (source, destination, flags, cancellable, NULL, NULL,
                      error);
}

gboolean
aml_fs_copy_file (GFile *source, GFile *destination, GFileCopyFlags flags,
                  GCancellable *cancellable, GError **error)
{
  return g_file_copy (source, destination, flags, cancellable, NULL, NULL,
                      error);
}

void
aml_fs_make_directory_async (GFile *file, int io_priority,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_file_make_directory_async (file, io_priority, cancellable, callback,
                               user_data);
}

gboolean
aml_fs_make_directory_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return g_file_make_directory_finish (file, result, error);
}

void
aml_fs_replace_contents_async (GFile *file, const char *contents, gsize length,
                               const char *etag, gboolean make_backup,
                               GFileCreateFlags flags, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data)
{
  g_file_replace_contents_async (file, contents, length, etag, make_backup,
                                 flags, cancellable, callback, user_data);
}

gboolean
aml_fs_replace_contents_finish (GFile *file, GAsyncResult *result,
                                char **new_etag, GError **error)
{
  return g_file_replace_contents_finish (file, result, new_etag, error);
}

void
aml_fs_replace_contents_bytes_async (GFile *file, GBytes *contents,
                                     const char *etag, gboolean make_backup,
                                     GFileCreateFlags flags,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  g_file_replace_contents_bytes_async (file, contents, etag, make_backup,
                                       flags, cancellable, callback, user_data);
}

gboolean
aml_fs_replace_contents_bytes_finish (GFile *file, GAsyncResult *result,
                                      char **new_etag, GError **error)
{
  return g_file_replace_contents_finish (file, result, new_etag, error);
}

void
aml_fs_copy_async (GFile *source, GFile *destination, GFileCopyFlags flags,
                   int io_priority, GCancellable *cancellable,
                   GFileProgressCallback progress_callback,
                   gpointer progress_data, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  g_file_copy_async (source, destination, flags, io_priority, cancellable,
                     progress_callback, progress_data, callback, user_data);
}

gboolean
aml_fs_copy_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return g_file_copy_finish (file, result, error);
}

void
aml_fs_delete_async (GFile *file, int io_priority, GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data)
{
  g_file_delete_async (file, io_priority, cancellable, callback, user_data);
}

gboolean
aml_fs_delete_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return g_file_delete_finish (file, result, error);
}

gboolean
aml_fs_directory_is_real (const char *path)
{
  g_auto (GStrv) parts = NULL;
  int current = -1;

  if (path == NULL || !g_path_is_absolute (path))
    return FALSE;

  parts = g_strsplit (path + 1, G_DIR_SEPARATOR_S, -1);
  current = open (G_DIR_SEPARATOR_S,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (current < 0)
    return FALSE;

  for (guint i = 0; parts[i] != NULL && parts[i][0] != '\0'; i++)
    {
      int next = openat (current, parts[i],
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        {
          gboolean missing = errno == ENOENT;
          close (current);
          return missing;
        }
      close (current);
      current = next;
    }

  close (current);
  return TRUE;
}

int
aml_fs_open_directory_nofollow (const char *path, GError **error)
{
  g_auto (GStrv) parts = NULL;
  int current = -1;

  if (path == NULL || !g_path_is_absolute (path))
    goto failed;
  parts = g_strsplit (path + 1, G_DIR_SEPARATOR_S, -1);
  current = open (G_DIR_SEPARATOR_S,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (current < 0)
    goto failed;
  for (guint i = 0; parts[i] != NULL && parts[i][0] != '\0'; i++)
    {
      int next = openat (current, parts[i],
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        goto failed;
      close (current);
      current = next;
    }
  return current;

failed:
  if (current >= 0)
    close (current);
  if (error != NULL)
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                 "cannot securely open directory \"%s\": %s", path,
                 g_strerror (errno));
  return -1;
}

int
aml_fs_open_temporary_file_nofollow (const char *directory, char **name,
                                     char **path, int *parent_fd,
                                     GError **error)
{
  int directory_fd = aml_fs_open_directory_nofollow (directory, error);

  if (directory_fd < 0)
    return -1;

  for (guint attempt = 0; attempt < 100; attempt++)
    {
      g_autofree char *candidate = g_strdup_printf (".aml-%08x%08x",
                                                    g_random_int (),
                                                    g_random_int ());
      int fd = openat (directory_fd, candidate,
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (fd >= 0)
        {
          if (name != NULL)
            *name = g_strdup (candidate);
          if (path != NULL)
            *path = g_build_filename (directory, candidate, NULL);
          if (parent_fd != NULL)
            *parent_fd = directory_fd;
          else
            close (directory_fd);
          return fd;
        }
      if (errno != EEXIST)
        break;
    }

  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
               "cannot create a temporary file in \"%s\": %s", directory,
               g_strerror (errno));
  close (directory_fd);
  return -1;
}

gboolean
aml_fs_unlink_at (int directory_fd, const char *name, GError **error)
{
  if (unlinkat (directory_fd, name, 0) != 0 && errno != ENOENT)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot remove \"%s\": %s", name, g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

gboolean
aml_fs_rename_at (int directory_fd, const char *source, const char *destination,
                  GError **error)
{
  if (renameat (directory_fd, source, directory_fd, destination) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot rename \"%s\" to \"%s\": %s", source,
                   destination, g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

gboolean
aml_fs_make_directory_tree_nofollow (const char *directory, const char *relative,
                                     int mode, GError **error)
{
  g_auto (GStrv) parts = g_strsplit (relative, "/", -1);
  int current = open (directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

  if (current < 0)
    goto failed;
  for (guint i = 0; parts[i] != NULL; i++)
    {
      int next;
      if (mkdirat (current, parts[i], mode) != 0 && errno != EEXIST)
        goto failed;
      next = openat (current, parts[i],
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        goto failed;
      close (current);
      current = next;
    }
  close (current);
  return TRUE;

failed:
  if (current >= 0)
    close (current);
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
               "cannot securely make \"%s\": %s", relative,
               g_strerror (errno));
  return FALSE;
}

int
aml_fs_open_file_nofollow (const char *directory, const char *relative,
                           int flags, int directory_mode, int mode,
                           GError **error)
{
  g_auto (GStrv) parts = g_strsplit (relative, "/", -1);
  int current = open (directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  guint last = 0;

  if (current < 0)
    goto failed;
  while (parts[last + 1] != NULL)
    last++;
  for (guint i = 0; i < last; i++)
    {
      int next;
      if (mkdirat (current, parts[i], directory_mode) != 0 && errno != EEXIST)
        goto failed;
      next = openat (current, parts[i],
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        goto failed;
      close (current);
      current = next;
    }

  {
    int fd = openat (current, parts[last], flags | O_CLOEXEC | O_NOFOLLOW,
                     mode);
    int saved_errno = errno;
    close (current);
    if (fd >= 0)
      return fd;
    errno = saved_errno;
  }
failed:
  if (current >= 0)
    close (current);
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
               "cannot securely open \"%s\": %s", relative,
               g_strerror (errno));
  return -1;
}

void
aml_fs_remove_tree (const char *path)
{
  GStatBuf stat_buf;

  if (path == NULL || g_lstat (path, &stat_buf) != 0)
    return;

  if (S_ISDIR (stat_buf.st_mode) && !S_ISLNK (stat_buf.st_mode))
    {
      g_autoptr (GDir) directory = g_dir_open (path, 0, NULL);
      const char *name = NULL;

      if (directory != NULL)
        while ((name = g_dir_read_name (directory)) != NULL)
          {
            g_autofree char *child = g_build_filename (path, name, NULL);
            aml_fs_remove_tree (child);
          }

      g_rmdir (path);
    }
  else
    {
      g_unlink (path);
    }
}

typedef struct {
  char *path;
  AmlTaskScope *scope;
} RemoveTreeData;

static void
remove_tree_data_free (RemoveTreeData *data)
{
  g_free (data->path);
  g_free (data);
}

static void
remove_tree_thread (GTask *task G_GNUC_UNUSED,
                    gpointer source_object G_GNUC_UNUSED,
                    gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
  RemoveTreeData *data = task_data;
  AmlTaskScope *scope = data->scope;

  if (aml_task_scope_return_error_if_cancelled (scope))
    return;

  aml_fs_remove_tree (data->path);

  if (!aml_task_scope_return_error_if_cancelled (scope))
    aml_task_scope_return_boolean (scope, TRUE);
}

AmlTaskScope *
aml_fs_remove_tree_async (const char *path, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  RemoveTreeData *data = g_new0 (RemoveTreeData, 1);

  data->path = g_strdup (path);
  data->scope = scope;
  g_task_set_task_data (task, data,
                        (GDestroyNotify) remove_tree_data_free);
  g_task_run_in_thread (task, remove_tree_thread);
  return g_steal_pointer (&scope);
}

gboolean
aml_fs_remove_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

typedef struct {
  GFile *source;
  GFile *destination;
  AmlTaskScope *scope;
} CopyTreeData;

static void
copy_tree_data_free (CopyTreeData *data)
{
  g_clear_object (&data->source);
  g_clear_object (&data->destination);
  g_free (data);
}

static gboolean
copy_tree_sync (GFile *source, GFile *destination, GCancellable *cancellable,
                GError **error)
{
  g_autoptr (GFileInfo) info = NULL;
  GFileType type;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  info = g_file_query_info (source,
                            G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                            G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            cancellable, error);
  if (info == NULL)
    return FALSE;

  type = g_file_info_get_file_type (info);
  if (type == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr (GFileEnumerator) enumerator = NULL;
      GFileInfo *child_info = NULL;

      if (!g_file_make_directory (destination, cancellable, error) &&
          !g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        return FALSE;
      g_clear_error (error);

      enumerator = g_file_enumerate_children (
        source, G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (enumerator == NULL)
        return FALSE;

      while ((child_info = g_file_enumerator_next_file (enumerator,
                                                         cancellable, error)) != NULL)
        {
          g_autoptr (GFile) child_source = g_file_get_child (
            source, g_file_info_get_name (child_info));
          g_autoptr (GFile) child_destination = g_file_get_child (
            destination, g_file_info_get_name (child_info));

          if (!copy_tree_sync (child_source, child_destination, cancellable,
                               error))
            {
              g_object_unref (child_info);
              return FALSE;
            }
          g_object_unref (child_info);
        }

      return *error == NULL;
    }

  if (type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      const char *target = g_file_info_get_symlink_target (info);
      g_autofree char *destination_path = g_file_get_path (destination);

      if (target == NULL || destination_path == NULL ||
          symlink (target, destination_path) != 0)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "cannot copy symlink: %s", g_strerror (errno));
          return FALSE;
        }
      return TRUE;
    }

  return g_file_copy (source, destination, G_FILE_COPY_NONE, cancellable,
                      NULL, NULL, error);
}

static void
copy_tree_thread (GTask *task G_GNUC_UNUSED,
                   gpointer source_object G_GNUC_UNUSED,
                  gpointer task_data, GCancellable *cancellable)
{
  CopyTreeData *data = task_data;
  AmlTaskScope *scope = data->scope;
  g_autoptr (GError) error = NULL;

  if (copy_tree_sync (data->source, data->destination, cancellable,
                      &error))
    aml_task_scope_return_boolean (scope, TRUE);
  else
    aml_task_scope_return_error (scope, g_steal_pointer (&error));
}

AmlTaskScope *
aml_fs_copy_tree_async (GFile *source, GFile *destination,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  CopyTreeData *data = g_new0 (CopyTreeData, 1);

  data->source = g_object_ref (source);
  data->destination = g_object_ref (destination);
  data->scope = scope;
  g_task_set_task_data (task, data, (GDestroyNotify) copy_tree_data_free);
  g_task_run_in_thread (task, copy_tree_thread);
  return g_steal_pointer (&scope);
}

gboolean
aml_fs_copy_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}
