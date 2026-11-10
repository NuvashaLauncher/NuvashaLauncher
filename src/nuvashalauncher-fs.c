/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-task-private.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

gboolean
nuvashalauncher_fs_replace_contents (const char *path, const void *contents,
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

  /* GLib preserves the permissions of an existing file when replacing it.
   * Enforce the requested mode explicitly so callers storing credentials do
   * not silently retain permissions such as 0644 from an older file. */
  if (g_chmod (path, mode) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot set permissions on \"%s\": %s", path,
                   g_strerror (saved_errno));
      return FALSE;
    }

  return TRUE;
}

gboolean
nuvashalauncher_fs_make_directory (const char *path, int mode, GError **error)
{
  if (g_mkdir_with_parents (path, mode) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot create \"%s\": %s", path, g_strerror (saved_errno));
      return FALSE;
    }

  return TRUE;
}

gboolean
nuvashalauncher_fs_get_contents_within (const char *path, goffset maximum,
                            char **contents, gsize *length, GError **error)
{
  struct stat info;
  int fd = -1;
  gsize used = 0;
  gsize capacity = 0;
  char *data = NULL;

  g_return_val_if_fail (contents != NULL, FALSE);
  *contents = NULL;
  if (length != NULL)
    *length = 0;

  if (path == NULL || maximum < 0)
    {
      errno = EINVAL;
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "cannot securely read \"%s\"", path != NULL ? path : "");
      return FALSE;
    }

  fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 || fstat (fd, &info) != 0)
    {
      int saved_errno = errno;
      if (fd >= 0)
        close (fd);
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot securely read \"%s\": %s", path,
                   g_strerror (saved_errno));
      return FALSE;
    }

  if (!S_ISREG (info.st_mode))
    {
      close (fd);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "cannot securely read \"%s\"", path);
      return FALSE;
    }

  if (info.st_size < 0 || info.st_size > maximum)
    {
      close (fd);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "file \"%s\" exceeds the permitted size", path);
      return FALSE;
    }

  capacity = (gsize) info.st_size + 1;
  data = g_malloc (capacity);
  while (used <= (gsize) maximum)
    {
      if (used >= capacity)
        {
          gsize next_capacity = MIN ((gsize) maximum + 1, MAX (capacity * 2, 4096));
          if (next_capacity <= used)
            next_capacity = (gsize) maximum + 1;
          capacity = next_capacity;
          data = g_realloc (data, capacity);
        }

      ssize_t got = read (fd, data + used, capacity - used);
      if (got == 0)
        break;
      if (got < 0 && errno == EINTR)
        continue;
      if (got < 0)
        {
          int saved_errno = errno;
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                       "cannot read \"%s\": %s", path, g_strerror (saved_errno));
          close (fd);
          g_free (data);
          return FALSE;
        }
      used += (gsize) got;
      if (used > (gsize) maximum)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                       "file \"%s\" exceeds the permitted size", path);
          close (fd);
          g_free (data);
          return FALSE;
        }
    }
  close (fd);
  if (used >= capacity)
    data = g_realloc (data, used + 1);
  data[used] = '\0';
  *contents = data;
  if (length != NULL)
    *length = used;
  return TRUE;
}

gboolean
nuvashalauncher_fs_unlink (const char *path, GError **error)
{
  if (g_unlink (path) != 0 && errno != ENOENT)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot remove \"%s\": %s", path, g_strerror (saved_errno));
      return FALSE;
    }
  return TRUE;
}

gboolean
nuvashalauncher_fs_rename (const char *source, const char *destination, GError **error)
{
  if (g_rename (source, destination) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot rename \"%s\" to \"%s\": %s", source,
                   destination, g_strerror (saved_errno));
      return FALSE;
    }
  return TRUE;
}

int
nuvashalauncher_fs_open_temporary_file (const char *directory, const char *prefix,
                            char **path, GError **error)
{
  g_autofree char *template =
    g_build_filename (directory, prefix != NULL ? prefix : ".nuvashalauncher-XXXXXX",
                      NULL);
  int fd = g_mkstemp (template);

  if (fd < 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot create temporary file in \"%s\": %s", directory,
                   g_strerror (saved_errno));
      return -1;
    }

  if (path != NULL)
    *path = g_steal_pointer (&template);
  return fd;
}

char *
nuvashalauncher_fs_make_temporary_directory (const char *parent, const char *prefix,
                                 GError **error)
{
  g_autofree char *template =
    g_build_filename (parent, prefix != NULL ? prefix : ".nuvashalauncher-XXXXXX", NULL);
  char *result = g_mkdtemp (template);

  if (result == NULL)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot create temporary directory in \"%s\": %s",
                   parent, g_strerror (saved_errno));
      return NULL;
    }
  if (g_chmod (result, 0700) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot secure temporary directory \"%s\": %s", result,
                   g_strerror (saved_errno));
      g_rmdir (result);
      return NULL;
    }
  return g_steal_pointer (&template);
}

gboolean
nuvashalauncher_fs_move_file (GFile *source, GFile *destination, GFileCopyFlags flags,
                  GCancellable *cancellable, GError **error)
{
  return g_file_move (source, destination, flags, cancellable, NULL, NULL,
                      error);
}

gboolean
nuvashalauncher_fs_copy_file (GFile *source, GFile *destination, GFileCopyFlags flags,
                  GCancellable *cancellable, GError **error)
{
  return g_file_copy (source, destination, flags, cancellable, NULL, NULL,
                      error);
}

void
nuvashalauncher_fs_make_directory_async (GFile *file, int io_priority,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_file_make_directory_async (file, io_priority, cancellable, callback,
                               user_data);
}

gboolean
nuvashalauncher_fs_make_directory_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return g_file_make_directory_finish (file, result, error);
}

void
nuvashalauncher_fs_replace_contents_async (GFile *file, const char *contents, gsize length,
                               const char *etag, gboolean make_backup,
                               GFileCreateFlags flags, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data)
{
  g_file_replace_contents_async (file, contents, length, etag, make_backup,
                                 flags, cancellable, callback, user_data);
}

gboolean
nuvashalauncher_fs_replace_contents_finish (GFile *file, GAsyncResult *result,
                                char **new_etag, GError **error)
{
  return g_file_replace_contents_finish (file, result, new_etag, error);
}

void
nuvashalauncher_fs_copy_async (GFile *source, GFile *destination, GFileCopyFlags flags,
                   int io_priority, GCancellable *cancellable,
                   GFileProgressCallback progress_callback,
                   gpointer progress_data, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  g_file_copy_async (source, destination, flags, io_priority, cancellable,
                     progress_callback, progress_data, callback, user_data);
}

gboolean
nuvashalauncher_fs_copy_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return g_file_copy_finish (file, result, error);
}

void
nuvashalauncher_fs_delete_async (GFile *file, int io_priority, GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data)
{
  g_file_delete_async (file, io_priority, cancellable, callback, user_data);
}

gboolean
nuvashalauncher_fs_delete_finish (GFile *file, GAsyncResult *result, GError **error)
{
  return g_file_delete_finish (file, result, error);
}

gboolean
nuvashalauncher_fs_directory_is_real (const char *path)
{
  g_auto (GStrv) parts = NULL;
  int current = -1;

  if (path == NULL || !g_path_is_absolute (path))
    {
      errno = EINVAL;
      return FALSE;
    }

  parts = g_strsplit (path + 1, G_DIR_SEPARATOR_S, -1);
  current = open (G_DIR_SEPARATOR_S,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (current < 0)
    return FALSE;

  for (guint i = 0; parts[i] != NULL; i++)
    {
      if (parts[i][0] == '\0')
        continue;
      if (g_str_equal (parts[i], ".") || g_str_equal (parts[i], ".."))
        {
          close (current);
          errno = EINVAL;
          return FALSE;
        }

      int next = openat (current, parts[i],
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        {
          if (errno == ENOENT)
            {
              for (guint j = i + 1; parts[j] != NULL; j++)
                {
                  if (parts[j][0] == '\0')
                    continue;
                  if (g_str_equal (parts[j], ".") || g_str_equal (parts[j], ".."))
                    {
                      close (current);
                      errno = EINVAL;
                      return FALSE;
                    }
                }
              close (current);
              return TRUE;
            }
          close (current);
          return FALSE;
        }
      close (current);
      current = next;
    }

  close (current);
  return TRUE;
}

int
nuvashalauncher_fs_open_directory_nofollow (const char *path, GError **error)
{
  g_auto (GStrv) parts = NULL;
  int current = -1;

  if (path == NULL || !g_path_is_absolute (path))
    {
      errno = EINVAL;
      goto failed;
    }
  parts = g_strsplit (path + 1, G_DIR_SEPARATOR_S, -1);
  current = open (G_DIR_SEPARATOR_S,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (current < 0)
    goto failed;
  for (guint i = 0; parts[i] != NULL; i++)
    {
      if (parts[i][0] == '\0')
        continue;
      if (g_str_equal (parts[i], ".") || g_str_equal (parts[i], ".."))
        {
          errno = EINVAL;
          goto failed;
        }
      int next = openat (current, parts[i],
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        goto failed;
      close (current);
      current = next;
    }
  return current;

failed:
  {
    int saved_errno = errno;
    if (current >= 0)
      close (current);
    errno = saved_errno;
    if (error != NULL)
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot securely open directory \"%s\": %s",
                   path != NULL ? path : "",
                   g_strerror (saved_errno));
  }
  return -1;
}

int
nuvashalauncher_fs_open_temporary_file_nofollow (const char *directory, char **name,
                                     char **path, int *parent_fd,
                                     GError **error)
{
  int directory_fd = nuvashalauncher_fs_open_directory_nofollow (directory, error);

  if (directory_fd < 0)
    return -1;

  for (guint attempt = 0; attempt < 100; attempt++)
    {
      g_autofree char *candidate = g_strdup_printf (".nuvashalauncher-%08x%08x",
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

  {
    int saved_errno = errno;
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                 "cannot create a temporary file in \"%s\": %s", directory,
                 g_strerror (saved_errno));
    close (directory_fd);
    return -1;
  }
}

gboolean
nuvashalauncher_fs_unlink_at (int directory_fd, const char *name, GError **error)
{
  if (unlinkat (directory_fd, name, 0) != 0 && errno != ENOENT)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot remove \"%s\": %s", name, g_strerror (saved_errno));
      return FALSE;
    }
  return TRUE;
}

gboolean
nuvashalauncher_fs_rename_at (int directory_fd, const char *source, const char *destination,
                  GError **error)
{
  if (renameat (directory_fd, source, directory_fd, destination) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "cannot rename \"%s\" to \"%s\": %s", source,
                   destination, g_strerror (saved_errno));
      return FALSE;
    }
  return TRUE;
}

gboolean
nuvashalauncher_fs_make_directory_tree_nofollow (const char *directory, const char *relative,
                                     int mode, GError **error)
{
  g_auto (GStrv) parts = NULL;
  int current = -1;

  if (relative == NULL || relative[0] == '\0')
    {
      errno = EINVAL;
      goto failed;
    }

  parts = g_strsplit (relative, "/", -1);
  if (parts[0] == NULL)
    {
      errno = EINVAL;
      goto failed;
    }
  for (guint i = 0; parts[i] != NULL; i++)
    {
      if (parts[i][0] == '\0' ||
          g_str_equal (parts[i], ".") ||
          g_str_equal (parts[i], ".."))
        {
          errno = EINVAL;
          goto failed;
        }
    }

  current = nuvashalauncher_fs_open_directory_nofollow (directory, error);
  if (current < 0)
    return FALSE;

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
  {
    int saved_errno = errno;
    if (current >= 0)
      close (current);
    errno = saved_errno;
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                 "cannot securely make \"%s\": %s",
                 relative != NULL ? relative : "",
                 g_strerror (saved_errno));
  }
  return FALSE;
}

int
nuvashalauncher_fs_open_file_nofollow (const char *directory, const char *relative,
                           int flags, int directory_mode, int mode,
                           GError **error)
{
  g_auto (GStrv) parts = NULL;
  int current = -1;
  guint last = 0;

  if (relative == NULL || relative[0] == '\0')
    {
      errno = EINVAL;
      goto failed;
    }

  parts = g_strsplit (relative, "/", -1);
  if (parts[0] == NULL)
    {
      errno = EINVAL;
      goto failed;
    }
  for (guint i = 0; parts[i] != NULL; i++)
    {
      if (parts[i][0] == '\0' ||
          g_str_equal (parts[i], ".") ||
          g_str_equal (parts[i], ".."))
        {
          errno = EINVAL;
          goto failed;
        }
    }

  current = nuvashalauncher_fs_open_directory_nofollow (directory, error);
  if (current < 0)
    return -1;

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
    current = -1;
    if (fd >= 0)
      return fd;
    errno = saved_errno;
  }
failed:
  {
    int saved_errno = errno;
    if (current >= 0)
      close (current);
    errno = saved_errno;
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                 "cannot securely open \"%s\": %s",
                 relative != NULL ? relative : "",
                 g_strerror (saved_errno));
  }
  return -1;
}
