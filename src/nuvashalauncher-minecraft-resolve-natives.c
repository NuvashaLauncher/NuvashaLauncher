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
      g_set_error (error, nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_UNSAFE_PATH,
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
  g_auto (GStrv) parts = g_strsplit (relative, "/", -1);
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

      if (g_file_query_exists (child, NULL))
        {
          if (!is_plain_directory (child, &local))
            {
              g_propagate_error (error, g_steal_pointer (&local));
              return FALSE;
            }

          continue;
        }

      if (!nuvashalauncher_fs_make_directory (path, 0755, &local))
        {
          g_propagate_error (error, g_steal_pointer (&local));
          return FALSE;
        }
    }

  return TRUE;
}

typedef struct
{
  GTask *task;
  nuvashalauncherTaskScope *scope;
  GPtrArray *jars; /* nuvashalauncherResolveFile*, a reference */
  char *directory;
  guint index;
  guint64 extracted_bytes;
} ExtractRequest;

/* One jar, open, while its entries are written one at a time. The bytes
   of the jar live as long as it does: libarchive reads out of them. */
typedef struct
{
  ExtractRequest *request;
  struct archive *archive;
  GBytes *bytes;
} ExtractStep;

typedef struct
{
  char *directory;
  char *relative;
  GBytes *data;
} NativeWrite;

static void extract_write_ready (GObject *source, GAsyncResult *result,
                                 gpointer user_data);

static void
native_write_free (NativeWrite *output)
{
  g_free (output->directory);
  g_free (output->relative);
  g_clear_pointer (&output->data, g_bytes_unref);
  g_free (output);
}

static void
native_write_thread (GTask *task,
                     gpointer source_object G_GNUC_UNUSED,
                     gpointer task_data,
                     GCancellable *cancellable)
{
  NativeWrite *output = task_data;
  g_autoptr (GError) error = NULL;
  gsize length = 0;
  const guint8 *data = g_bytes_get_data (output->data, &length);
  int fd = nuvashalauncher_fs_open_file_nofollow (
    output->directory, output->relative,
    O_WRONLY | O_CREAT | O_TRUNC, 0755, 0644, &error);

  if (fd < 0)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  for (gsize written = 0; written < length;)
    {
      ssize_t count = write (fd, data + written, length - written);

      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        {
          g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "cannot write native file \"%s\": %s",
                       output->relative, g_strerror (errno));
          close (fd);
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }
      written += (gsize) count;
    }

  if (close (fd) != 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "cannot close native file \"%s\": %s",
                   output->relative, g_strerror (errno));
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "native extraction was cancelled");
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
native_write_async (ExtractStep *step, const char *relative, GBytes *data)
{
  NativeWrite *output = g_new0 (NativeWrite, 1);
  GTask *task = g_task_new (NULL,
                            g_task_get_cancellable (step->request->task),
                            extract_write_ready, step);

  output->directory = g_strdup (step->request->directory);
  output->relative = g_strdup (relative);
  output->data = g_bytes_ref (data);
  g_task_set_task_data (task, output, (GDestroyNotify) native_write_free);
  g_task_run_in_thread (task, native_write_thread);
  g_object_unref (task);
}

static void
extract_request_free (ExtractRequest *request)
{
  nuvashalauncher_task_scope_unref (request->scope);
  g_clear_object (&request->task);
  g_clear_pointer (&request->jars, g_ptr_array_unref);
  g_free (request->directory);
  g_free (request);
}

static void
extract_step_free (ExtractStep *step)
{
  if (step->archive != NULL)
    archive_read_free (step->archive);

  g_clear_pointer (&step->bytes, g_bytes_unref);
  g_free (step);
}

static void extract_next_jar (ExtractRequest *request);
static void extract_entries (ExtractStep *step);
static void extract_jar_info_ready (GObject *source, GAsyncResult *result,
                                    gpointer user_data);

/* Ends the whole request. Consumes @error, which may be NULL. */
static void
extract_finished (ExtractRequest *request, GError *error)
{
  if (error != NULL)
    nuvashalauncher_task_scope_return_error (request->scope, error);
  else
    nuvashalauncher_task_scope_return_boolean (request->scope, TRUE);

  extract_request_free (request);
}

static void
extract_read_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;

  ExtractStep *step = user_data;
  nuvashalauncherResolveFile *jar =
    g_ptr_array_index (step->request->jars, step->request->index);

  bytes = g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  if (bytes == NULL)
    {
      extract_finished (step->request, g_steal_pointer (&error));
      extract_step_free (step);
      return;
    }

  /* The archive reads out of the bytes, so they stay until the jar is
     done with them. */
  step->bytes = g_steal_pointer (&bytes);
  step->archive = archive_read_new ();

  if (archive_read_support_format_zip (step->archive) != ARCHIVE_OK ||
      archive_read_open_memory (step->archive,
                                g_bytes_get_data (step->bytes, NULL),
                                g_bytes_get_size (step->bytes)) != ARCHIVE_OK)
    {
      extract_finished (
          step->request,
          g_error_new (nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_INVALID,
                       "\"%s\" is not a jar nuvashalauncher can read: %s", jar->path,
                       archive_error_string (step->archive)));
      extract_step_free (step);
      return;
    }

  step->request->extracted_bytes += g_bytes_get_size (step->bytes);

  extract_entries (step);
}

/* One entry has been written: back into the jar for the next one. */
static void
extract_write_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  ExtractStep *step = user_data;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      extract_finished (step->request, g_steal_pointer (&error));
      extract_step_free (step);
      return;
    }

  extract_entries (step);
}

/* Reads the whole body of the current entry into @error on failure. */
static gboolean
read_entry (struct archive *archive, GBytes **data, GError **error)
{
  g_autoptr (GByteArray) buffer = g_byte_array_new ();
  const void *block = NULL;
  gsize size = 0;
  la_int64_t offset = 0;
  int status;

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
          g_set_error (error, nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_INVALID,
                       "cannot read an entry of the jar: %s",
                       archive_error_string (archive));
          return FALSE;
        }

      if (size > 0)
        g_byte_array_append (buffer, block, size);

      if (buffer->len > MAX_NATIVE_ENTRY_SIZE)
        {
          g_set_error (error, nuvashalauncher_RESOLVE_ERROR,
                       nuvashalauncher_RESOLVE_ERROR_INVALID,
                       "a native entry is larger than %u MiB",
                       MAX_NATIVE_ENTRY_SIZE / (1024 * 1024));
          return FALSE;
        }
    }

  if (buffer->len == 0)
    {
      /* An empty entry is an empty file, not nothing at all. */
      g_byte_array_unref (g_steal_pointer (&buffer));
      *data = g_bytes_new_static ("", 0);
      return TRUE;
    }

  gsize length = buffer->len;
  *data = g_bytes_new_take (g_byte_array_free (g_steal_pointer (&buffer), FALSE),
                            length);

  return TRUE;
}

/* Walks the entries of the jar of @step and starts one write at a time,
   coming back here when it lands. */
static void
extract_entries (ExtractStep *step)
{
  ExtractRequest *request = step->request;
  nuvashalauncherResolveFile *jar = g_ptr_array_index (request->jars, request->index);
  struct archive_entry *entry = NULL;
  int status;

  while ((status = archive_read_next_header (step->archive, &entry)) == ARCHIVE_OK)
    {
      const char *name = archive_entry_pathname (entry);
      g_autofree char *relative = NULL;
      g_autofree char *path = NULL;
      g_autoptr (GBytes) data = NULL;
      g_autoptr (GError) error = NULL;
      gsize length = 0;
      mode_t filetype = archive_entry_filetype (entry);

      if (name == NULL || entry_excluded (jar->exclude, name))
        continue;

      /* A directory entry ends in a slash, which is not a component of
         its own to test. */
      length = strlen (name);

      while (length > 0 && name[length - 1] == '/')
        length--;

      relative = g_strndup (name, length);

      if (!nuvashalauncher_resolve_entry_name_is_safe (relative))
        {
          extract_finished (
              request,
              g_error_new (nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_UNSAFE_PATH,
                           "\"%s\" holds an entry that would be written "
                           "outside the natives directory: \"%s\"",
                           jar->path, name));
          extract_step_free (step);
          return;
        }

      path = g_build_filename (request->directory, relative, NULL);

      if (filetype == AE_IFDIR)
        {
          if (!nuvashalauncher_resolve_ensure_directory (path, &error))
            {
              extract_finished (request, g_steal_pointer (&error));
              extract_step_free (step);
              return;
            }

          continue;
        }

      /* Only regular files are written. A symlink, a hard link or a
         device inside a jar is skipped: honouring one would let a jar
         put a name of the natives directory somewhere else on the
         disk, or run something that was never meant to be run. */
      if (filetype != AE_IFREG)
        continue;

      if (!make_parents (request->directory, relative, &error))
        {
          extract_finished (request, g_steal_pointer (&error));
          extract_step_free (step);
          return;
        }

      if (!read_entry (step->archive, &data, &error))
        {
          extract_finished (request, g_steal_pointer (&error));
          extract_step_free (step);
          return;
        }

      if (request->extracted_bytes > MAX_NATIVE_TOTAL_SIZE ||
          g_bytes_get_size (data) > MAX_NATIVE_TOTAL_SIZE - request->extracted_bytes)
        {
          extract_finished (
              request,
              g_error_new (nuvashalauncher_RESOLVE_ERROR,
                           nuvashalauncher_RESOLVE_ERROR_INVALID,
                           "native libraries are larger than %u MiB",
                           MAX_NATIVE_TOTAL_SIZE / (1024 * 1024)));
          extract_step_free (step);
          return;
        }

       native_write_async (step, relative, data);
       return;
    }

  if (status != ARCHIVE_EOF)
    {
      extract_finished (
          request,
          g_error_new (nuvashalauncher_RESOLVE_ERROR,
                       nuvashalauncher_RESOLVE_ERROR_INVALID,
                       "cannot read native archive \"%s\": %s",
                       jar->path, archive_error_string (step->archive)));
      extract_step_free (step);
      return;
    }

  /* The jar is done: on to the next one. */
  request->index++;
  extract_step_free (step);
  extract_next_jar (request);
}

static void
extract_next_jar (ExtractRequest *request)
{
  nuvashalauncherResolveFile *jar;
  ExtractStep *step;
  g_autoptr (GFile) file = NULL;

  if (request->index >= request->jars->len)
    {
      extract_finished (request, NULL);
      return;
    }

  jar = g_ptr_array_index (request->jars, request->index);
  file = g_file_new_for_path (jar->path);

  step = g_new0 (ExtractStep, 1);
  step->request = request;

  g_file_query_info_async (file, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                           G_FILE_ATTRIBUTE_STANDARD_SIZE,
                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                           G_PRIORITY_DEFAULT,
                           g_task_get_cancellable (request->task),
                           extract_jar_info_ready, step);
}

static void
extract_jar_info_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ExtractStep *step = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInfo) info =
    g_file_query_info_finish (G_FILE (source), result, &error);

  if (info == NULL)
    {
      extract_finished (step->request, g_steal_pointer (&error));
      extract_step_free (step);
      return;
    }

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR ||
      g_file_info_get_size (info) > MAX_NATIVE_TOTAL_SIZE)
    {
      extract_finished (
          step->request,
          g_error_new_literal (nuvashalauncher_RESOLVE_ERROR,
                               nuvashalauncher_RESOLVE_ERROR_INVALID,
                               "a native archive is too large or is not a regular file"));
      extract_step_free (step);
      return;
    }

  g_file_load_bytes_async (G_FILE (source),
                           g_task_get_cancellable (step->request->task),
                           extract_read_ready, step);
}

nuvashalauncherTaskScope *
nuvashalauncher_resolve_extract_natives_async (GPtrArray         *jars,
                                   const char        *natives_directory,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  ExtractRequest *request;

  g_return_val_if_fail (jars != NULL, NULL);
  g_return_val_if_fail (natives_directory != NULL, NULL);
  g_return_val_if_fail (g_path_is_absolute (natives_directory), NULL);

  request = g_new0 (ExtractRequest, 1);

  /* Keep a reference separate from the handle returned to the caller. The
     completion callback may release that handle before this request has
     finished cleaning up its task state. */
  request->task = g_object_ref (nuvashalauncher_task_scope_task (scope));
  request->scope = nuvashalauncher_task_scope_ref (scope);
  g_task_set_check_cancellable (request->task, FALSE);
  request->jars = g_ptr_array_ref (jars);
  request->directory = g_strdup (natives_directory);

  extract_next_jar (request);

  return g_steal_pointer (&scope);
}

gboolean
nuvashalauncher_resolve_extract_natives_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
