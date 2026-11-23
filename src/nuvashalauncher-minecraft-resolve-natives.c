/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Safe asynchronous extraction of native libraries from Minecraft JARs. */

#include "nuvashalauncher-minecraft-resolve-private.h"
#include "nuvashalauncher-task-private.h"
#include "nuvashalauncher-fs.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_NATIVE_ENTRY_SIZE (64 * 1024 * 1024)
#define MAX_NATIVE_TOTAL_SIZE (256 * 1024 * 1024)

/* ---------------------------------------------------------- *
 * Unpacking the natives
 * ---------------------------------------------------------- */

/* TRUE when @name is @prefix, or lies below it when that is a
   directory. Mojang writes the prefixes with a trailing slash, which the
   directory entry of the jar itself does not have, so both spellings are
   tried. */
static gboolean
excluded_by (const char *prefix, const char *name)
{
  gsize length = strlen (prefix);

  if (g_str_has_prefix (name, prefix))
    return TRUE;

  return length > 1 && prefix[length - 1] == '/' &&
         strlen (name) == length - 1 &&
         strncmp (name, prefix, length - 1) == 0;
}

/* An entry is dropped when one of the exclude prefixes of its jar covers
   it. A jar whose document lists nothing is taken to exclude META-INF:
   no launcher wants the signatures and the metadata of a jar inside the
   natives the game loads. */
static gboolean
entry_excluded (GPtrArray *exclude, const char *name)
{
  if (exclude == NULL)
    return excluded_by ("META-INF/", name);

  for (guint i = 0; i < exclude->len; i++)
    {
      if (excluded_by (g_ptr_array_index (exclude, i), name))
        return TRUE;
    }

  return FALSE;
}

/* TRUE when @file is a directory and not something that only looks like
   one. A symlink is refused: an entry of a jar must never be able to
   make the next entry land outside the directory it is written to. */
static gboolean
is_plain_directory (GFile *file, GError **error)
{
  g_autoptr (GFileInfo) info = g_file_query_info (
      file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      NULL, error);

  if (info == NULL)
    return FALSE;

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
    {
      g_set_error (error, NUVASHALAUNCHER_RESOLVE_ERROR, NUVASHALAUNCHER_RESOLVE_ERROR_UNSAFE_PATH,
                   "\"%s\" is not a directory", g_file_peek_path (file));
      return FALSE;
    }

  return TRUE;
}

/* Creates every directory of @relative below @root, refusing anything
   that is not a plain directory on the way. */
static gboolean
make_parents (const char *root, const char *relative, GError **error)
{
  GStrv parts = g_strsplit (relative, "/", -1);
  g_autofree char *path = g_strdup (root);
  g_autoptr (GFile) child = NULL;

  /* The last part is the file itself. */
  for (guint i = 0; parts[i + 1] != NULL; i++)
    {
      g_autofree char *next = g_build_filename (path, parts[i], NULL);
      g_autoptr (GError) local = NULL;

      g_free (path);
      path = g_steal_pointer (&next);

      g_clear_object (&child);
      child = g_file_new_for_path (path);

      if (g_file_query_file_type (child, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                  NULL) != G_FILE_TYPE_UNKNOWN)
        {
          if (!is_plain_directory (child, &local))
            {
              g_propagate_error (error, g_steal_pointer (&local));
              g_strfreev (parts);
              return FALSE;
            }

          continue;
        }

      if (!nuvashalauncher_fs_make_directory (path, 0755, &local))
        {
          g_propagate_error (error, g_steal_pointer (&local));
          g_strfreev (parts);
          return FALSE;
        }
    }

  g_strfreev (parts);
  return TRUE;
}

typedef struct
{
  NuvashaLauncherTaskScope *scope;
  GPtrArray *jars; /* NuvashaLauncherResolveFile*, a reference */
  char *directory;
  guint64 extracted_bytes;
} ExtractRequest;

static void
extract_request_free (ExtractRequest *request)
{
  nuvashalauncher_task_scope_unref (request->scope);
  g_clear_pointer (&request->jars, g_ptr_array_unref);
  g_free (request->directory);
  g_free (request);
}

static gboolean
write_all_bytes (int fd, const guint8 *data, gsize length, const char *relative,
                 GError **error)
{
  for (gsize written = 0; written < length;)
    {
      ssize_t count = write (fd, data + written, length - written);

      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "cannot write native file \"%s\": %s",
                       relative, g_strerror (errno));
          return FALSE;
        }
      written += (gsize) count;
    }

  return TRUE;
}

static gboolean
extract_single_entry (ExtractRequest *request, struct archive *archive,
                      const char *relative, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  const void *block = NULL;
  size_t size = 0;
  la_int64_t offset = 0;
  guint64 entry_bytes = 0;
  int status;
  int fd;

  fd = nuvashalauncher_fs_open_file_nofollow (
    request->directory, relative,
    O_WRONLY | O_CREAT | O_TRUNC, 0755, 0644, error);
  if (fd < 0)
    return FALSE;

  for (;;)
    {
      block = NULL;
      size = 0;
      offset = 0;

      status = archive_read_data_block (archive, &block, &size, &offset);
      if (status == ARCHIVE_EOF)
        break;

      if (status < ARCHIVE_WARN)
        {
          g_set_error (error, NUVASHALAUNCHER_RESOLVE_ERROR, NUVASHALAUNCHER_RESOLVE_ERROR_INVALID,
                       "cannot read an entry of the jar: %s",
                       archive_error_string (archive));
          close (fd);
          return FALSE;
        }

      if (size > MAX_NATIVE_ENTRY_SIZE - entry_bytes)
        {
          g_set_error (error, NUVASHALAUNCHER_RESOLVE_ERROR,
                       NUVASHALAUNCHER_RESOLVE_ERROR_INVALID,
                       "a native entry is larger than %u MiB",
                       MAX_NATIVE_ENTRY_SIZE / (1024 * 1024));
          close (fd);
          return FALSE;
        }

      if (size > MAX_NATIVE_TOTAL_SIZE - request->extracted_bytes)
        {
          g_set_error (error, NUVASHALAUNCHER_RESOLVE_ERROR,
                       NUVASHALAUNCHER_RESOLVE_ERROR_INVALID,
                       "native libraries are larger than %u MiB",
                       MAX_NATIVE_TOTAL_SIZE / (1024 * 1024));
          close (fd);
          return FALSE;
        }

      if (size > 0 &&
          !write_all_bytes (fd, block, size, relative, &local_error))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          close (fd);
          return FALSE;
        }

      entry_bytes += size;
      request->extracted_bytes += size;
    }

  if (close (fd) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "cannot close native file \"%s\": %s",
                   relative, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

static gboolean
extract_single_jar (ExtractRequest *request, const NuvashaLauncherResolveFile *jar,
                    GCancellable *cancellable, GError **error)
{
  g_autoptr (GFile) file = g_file_new_for_path (jar->path);
  g_autoptr (GFileInfo) info = NULL;
  struct archive *archive = NULL;
  struct archive_entry *entry = NULL;
  int status;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "native extraction was cancelled");
      return FALSE;
    }

  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                            G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            cancellable, error);
  if (info == NULL)
    return FALSE;

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR ||
      g_file_info_get_size (info) > MAX_NATIVE_TOTAL_SIZE)
    {
      g_set_error_literal (error, NUVASHALAUNCHER_RESOLVE_ERROR,
                           NUVASHALAUNCHER_RESOLVE_ERROR_INVALID,
                           "a native archive is too large or is not a regular file");
      return FALSE;
    }

  archive = archive_read_new ();
  if (archive_read_support_format_zip (archive) != ARCHIVE_OK ||
      archive_read_open_filename (archive, jar->path, 64 * 1024) != ARCHIVE_OK)
    {
      g_set_error (error, NUVASHALAUNCHER_RESOLVE_ERROR, NUVASHALAUNCHER_RESOLVE_ERROR_INVALID,
                   "\"%s\" is not a jar nuvashalauncher can read: %s",
                   jar->path, archive_error_string (archive));
      archive_read_free (archive);
      return FALSE;
    }

  while ((status = archive_read_next_header (archive, &entry)) == ARCHIVE_OK)
    {
      const char *name = archive_entry_pathname (entry);
      g_autofree char *relative = NULL;
      gsize length = 0;
      mode_t filetype = archive_entry_filetype (entry);

      if (g_cancellable_is_cancelled (cancellable))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "native extraction was cancelled");
          archive_read_free (archive);
          return FALSE;
        }

      if (name == NULL || entry_excluded (jar->exclude, name))
        {
          archive_read_data_skip (archive);
          continue;
        }

      /* A directory entry ends in a slash, which is not a component of
         its own to test. */
      length = strlen (name);
      while (length > 0 && name[length - 1] == '/')
        length--;

      relative = g_strndup (name, length);
      if (!nuvashalauncher_resolve_entry_name_is_safe (relative))
        {
          g_set_error (error, NUVASHALAUNCHER_RESOLVE_ERROR,
                       NUVASHALAUNCHER_RESOLVE_ERROR_UNSAFE_PATH,
                       "\"%s\" holds an entry that would be written "
                       "outside the natives directory: \"%s\"",
                       jar->path, name);
          archive_read_free (archive);
          return FALSE;
        }

      if (filetype == AE_IFDIR)
        {
          if (!nuvashalauncher_fs_make_directory_tree_nofollow (
                request->directory, relative, 0755, error))
            {
              archive_read_free (archive);
              return FALSE;
            }
          continue;
        }

      /* Only regular files are written. A symlink, a hard link or a
         device inside a jar is skipped: honouring one would let a jar
         put a name of the natives directory somewhere else on the
         disk, or run something that was never meant to be run. */
      if (filetype != AE_IFREG)
        {
          archive_read_data_skip (archive);
          continue;
        }

      if (!make_parents (request->directory, relative, error) ||
          !extract_single_entry (request, archive, relative, error))
        {
          archive_read_free (archive);
          return FALSE;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, NUVASHALAUNCHER_RESOLVE_ERROR, NUVASHALAUNCHER_RESOLVE_ERROR_INVALID,
                   "cannot read native archive \"%s\": %s",
                   jar->path, archive_error_string (archive));
      archive_read_free (archive);
      return FALSE;
    }

  archive_read_free (archive);
  return TRUE;
}

static void
extract_natives_worker_thread (GTask *task,
                               gpointer source_object G_GNUC_UNUSED,
                               gpointer task_data,
                               GCancellable *cancellable)
{
  ExtractRequest *request = task_data;
  g_autoptr (GError) error = NULL;

  for (guint i = 0; i < request->jars->len; i++)
    {
      const NuvashaLauncherResolveFile *jar = g_ptr_array_index (request->jars, i);

      if (!extract_single_jar (request, jar, cancellable, &error))
        {
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }
    }

  g_task_return_boolean (task, TRUE);
}

static void
extract_natives_worker_done (GObject *source G_GNUC_UNUSED,
                             GAsyncResult *result,
                             gpointer user_data)
{
  ExtractRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  if (g_task_propagate_boolean (G_TASK (result), &error))
    nuvashalauncher_task_scope_return_boolean (request->scope, TRUE);
  else
    nuvashalauncher_task_scope_return_error (request->scope,
                                             g_steal_pointer (&error));
}

NuvashaLauncherTaskScope *
nuvashalauncher_resolve_extract_natives_async (GPtrArray         *jars,
                                   const char        *natives_directory,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  ExtractRequest *request;
  GTask *worker;

  g_return_val_if_fail (jars != NULL, NULL);
  g_return_val_if_fail (natives_directory != NULL, NULL);
  g_return_val_if_fail (g_path_is_absolute (natives_directory), NULL);

  request = g_new0 (ExtractRequest, 1);
  request->scope = nuvashalauncher_task_scope_ref (scope);
  request->jars = g_ptr_array_ref (jars);
  request->directory = g_strdup (natives_directory);

  g_task_set_check_cancellable (nuvashalauncher_task_scope_task (scope), FALSE);
  worker = g_task_new (NULL, nuvashalauncher_task_scope_get_cancellable (scope),
                       extract_natives_worker_done, request);
  g_task_set_task_data (worker, request, (GDestroyNotify) extract_request_free);
  g_task_run_in_thread (worker, extract_natives_worker_thread);
  g_object_unref (worker);

  return g_steal_pointer (&scope);
}

gboolean
nuvashalauncher_resolve_extract_natives_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
