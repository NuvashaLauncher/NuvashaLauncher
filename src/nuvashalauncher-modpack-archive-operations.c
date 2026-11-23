#define _GNU_SOURCE

/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Modpack file verification and override extraction. Archive validation and
 * manifest parsing remain in nuvashalauncher-modpack-archive.c. */

#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-modpack-private.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-task-private.h"

#include <archive.h>
#include <fcntl.h>
#include <unistd.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  char *path;
  char *sha1;
  char *sha512;
  NuvashaLauncherTaskScope *scope;
} VerifyRun;

static void
verify_run_free (VerifyRun *run)
{
  if (run == NULL)
    return;

  g_free (run->path);
  g_free (run->sha1);
  g_free (run->sha512);
  g_clear_pointer (&run->scope, nuvashalauncher_task_scope_unref);
  g_free (run);
}

static void
verify_file_thread (GTask *task, gpointer source_object G_GNUC_UNUSED,
                    gpointer task_data, GCancellable *cancellable)
{
  VerifyRun *run = task_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = g_file_new_for_path (run->path);
  g_autoptr (GFileInputStream) stream = NULL;
  g_autoptr (GChecksum) sha1 = NULL;
  g_autoptr (GChecksum) sha512 = NULL;
  g_autofree guchar *buffer = NULL;

  stream = g_file_read (file, cancellable, &error);
  if (stream == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  sha1 = g_checksum_new (G_CHECKSUM_SHA1);
  sha512 = g_checksum_new (G_CHECKSUM_SHA512);
  buffer = g_malloc (256 * 1024);

  for (;;)
    {
      gssize read_bytes = g_input_stream_read (G_INPUT_STREAM (stream), buffer,
                                               256 * 1024, cancellable, &error);

      if (read_bytes < 0)
        {
          g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }

      if (read_bytes == 0)
        break;

      g_checksum_update (sha1, buffer, (gsize) read_bytes);
      g_checksum_update (sha512, buffer, (gsize) read_bytes);
    }

  g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);

  if (g_ascii_strcasecmp (run->sha1, g_checksum_get_string (sha1)) != 0)
    {
      g_task_return_new_error (task, NUVASHALAUNCHER_MODPACK_ERROR,
                               NUVASHALAUNCHER_MODPACK_ERROR_HASH,
                               "\"%s\" arrived with a different %s than the index says",
                               run->path, "sha1");
      return;
    }

  if (g_ascii_strcasecmp (run->sha512, g_checksum_get_string (sha512)) != 0)
    {
      g_task_return_new_error (task, NUVASHALAUNCHER_MODPACK_ERROR,
                               NUVASHALAUNCHER_MODPACK_ERROR_HASH,
                               "\"%s\" arrived with a different %s than the index says",
                               run->path, "sha512");
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
verify_file_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  VerifyRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (g_task_propagate_boolean (G_TASK (result), &error))
    nuvashalauncher_task_scope_return_boolean (run->scope, TRUE);
  else
    nuvashalauncher_task_scope_return_error (run->scope, g_steal_pointer (&error));

  verify_run_free (run);
}

/* Both hashes of the manifest, out of the bytes on the disk. */
NuvashaLauncherTaskScope *
nuvashalauncher_modpack_verify_file_async (const NuvashaLauncherModpackFile *file,
                               const char *path,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autoptr (GTask) worker = NULL;
  VerifyRun *run = NULL;

  g_return_val_if_fail (file != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  if (file->sha1 == NULL || file->sha512 == NULL)
    {
      nuvashalauncher_task_scope_return_error_deferred (
        scope,
        g_error_new (NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_HASH,
                     "\"%s\" does not carry the two hashes the format "
                     "requires", file->path != NULL ? file->path : "file"));
      return g_steal_pointer (&scope);
    }

  run = g_new0 (VerifyRun, 1);
  run->path = g_strdup (path);
  run->sha1 = g_strdup (file->sha1);
  run->sha512 = g_strdup (file->sha512);
  run->scope = nuvashalauncher_task_scope_ref (scope);

  worker = g_task_new (NULL, nuvashalauncher_task_scope_get_cancellable (scope),
                       verify_file_done, run);
  g_task_set_task_data (worker, run, NULL);
  g_task_run_in_thread (worker, verify_file_thread);

  return g_steal_pointer (&scope);
}

gboolean
nuvashalauncher_modpack_verify_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Extraction of the overrides
 * ---------------------------------------------------------- */

static gboolean
extract_entry (struct archive *archive,
               struct archive_entry *header,
               const char *directory,
               const char *prefix,
               guint64 *used,
               GError **error)
{
  const char *name = archive_entry_pathname (header);
  g_autofree char *relative = NULL;
  FILE *out = NULL;

  if (name == NULL)
    {
      g_set_error_literal (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_FORMAT,
                           "an override has no pathname");
      return FALSE;
    }

  if (g_str_equal (prefix, "icon.png"))
    {
      if (!g_str_equal (name, prefix))
        return TRUE;
      relative = g_strdup (name);
    }
  else
    {
      if (!g_str_has_prefix (name, prefix))
        return TRUE;
      relative = g_strdup (name + strlen (prefix));
    }

  /* The prefix itself is no file. */
  if (relative[0] == '\0')
    return TRUE;

  /* A directory ends in a /: no name of its own to check against the
   * safety rules, only its contents. */
  {
    gsize length = strlen (relative);

    if (length > 0 && relative[length - 1] == '/')
      relative[length - 1] = '\0';
  }

  /* The prefix itself is no file. */
  if (relative[0] == '\0')
    return TRUE;

  /* nuvashalauncher's content indexes are launcher metadata, never game files. */
  if (g_str_equal (relative, ".index") ||
      g_str_has_prefix (relative, ".index/") ||
      strstr (relative, "/.index/") != NULL ||
      g_str_has_suffix (relative, "/.index"))
    return TRUE;

  if (!nuvashalauncher_modpack_path_is_safe (relative))
    {
      g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_PATH,
                   "the overrides hold \"%s\", which is not a path the "
                   "instance can hold",
                   name);
      return FALSE;
    }

  if (archive_entry_filetype (header) == AE_IFDIR)
    {
      g_autoptr (GError) local_error = NULL;
      if (!nuvashalauncher_fs_make_directory_tree_nofollow (directory, relative, 0755,
                                                &local_error))
        {
          g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_IO,
                       "\"%s\" cannot be made: %s", name,
                       local_error != NULL ? local_error->message : g_strerror (errno));
          return FALSE;
        }
      return TRUE;
    }

  if (archive_entry_filetype (header) != AE_IFREG)
    {
      g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_FORMAT,
                   "\"%s\" is not a file", name);
      return FALSE;
    }

  {
    g_autoptr (GError) local_error = NULL;
    int fd = nuvashalauncher_fs_open_file_nofollow (directory, relative,
                                       O_WRONLY | O_CREAT | O_TRUNC, 0755, 0644,
                                       &local_error);
    if (fd >= 0)
      out = fdopen (fd, "wb");
    if (out == NULL && fd >= 0)
      close (fd);
    if (out == NULL)
      {
        g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_IO,
                     "\"%s\" cannot be written: %s", name,
                     local_error != NULL ? local_error->message : g_strerror (errno));
        return FALSE;
      }
  }

  for (;;)
    {
      const void *block = NULL;
      size_t size = 0;
      la_int64_t offset = 0;
      int status = archive_read_data_block (archive, &block, &size, &offset);

      if (status == ARCHIVE_EOF)
        break;

      if (status != ARCHIVE_OK)
        {
          fclose (out);
      g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_ARCHIVE,
                       "\"%s\" cannot be read out of the archive: %s", name,
                       archive_error_string (archive));
          return FALSE;
        }

      if (size != 0)
        {
          /* The bomb protection runs on the bytes that did inflate, not
           * on what the entry announced. */
          if (*used > nuvashalauncher_modpack_get_content_limit () ||
              (guint64) size > nuvashalauncher_modpack_get_content_limit () - *used)
            {
              fclose (out);
              g_set_error_literal (error, NUVASHALAUNCHER_MODPACK_ERROR,
                                   NUVASHALAUNCHER_MODPACK_ERROR_BOMB,
                                   "the overrides decode past the size one "
                                   "pack carries");
              return FALSE;
            }

          if (fwrite (block, 1, size, out) != size)
            {
              fclose (out);
              g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_IO,
                           "\"%s\" cannot be written: %s", name,
                           g_strerror (errno));
              return FALSE;
            }

          *used += size;
        }
    }

  if (fclose (out) != 0)
    {
      g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_IO,
                   "\"%s\" cannot be closed: %s", name, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

/* Single archive pass for icon.png, overrides/, and client-overrides/.
 * client-overrides/ always wins over overrides/ regardless of entry order. */
static gboolean
extract_layers (const char *archive_path, const char *directory,
                guint64 *used, GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  g_autoptr (GHashTable) client_overridden =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  int status = ARCHIVE_OK;
  guint paths = 0;

  archive = archive_read_new ();

  int fd = open (archive_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    {
      int saved_errno = errno;
      g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_ARCHIVE,
                   "cannot open \"%s\": %s", archive_path, g_strerror (saved_errno));
      archive_read_free (archive);
      return FALSE;
    }

  archive_read_support_filter_all (archive);
  if (archive_read_support_format_zip (archive) != ARCHIVE_OK ||
      archive_read_open_fd (archive, fd, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_ARCHIVE,
                   "the pack cannot be opened: %s",
                   archive_error_string (archive));
      close (fd);
      archive_read_free (archive);
      return FALSE;
    }

  while ((status = archive_read_next_header (archive, &header)) == ARCHIVE_OK ||
         status == ARCHIVE_WARN)
    {
      if (++paths > NUVASHALAUNCHER_MODPACK_MAX_ENTRIES)
        {
          g_set_error_literal (error, NUVASHALAUNCHER_MODPACK_ERROR,
                               NUVASHALAUNCHER_MODPACK_ERROR_BOMB,
                               "the archive has more entries than one pack");
          archive_read_close (archive);
          close (fd);
          fd = -1;
          archive_read_free (archive);
          return FALSE;
        }

      if (!nuvashalauncher_modpack_archive_entry_is_safe (
            archive_entry_pathname (header), archive_entry_filetype (header),
            archive_entry_hardlink (header), error))
        {
          archive_read_close (archive);
          close (fd);
          fd = -1;
          archive_read_free (archive);
          return FALSE;
        }

      const char *name = archive_entry_pathname (header);
      const char *prefix = NULL;

      if (name != NULL && g_str_equal (name, "icon.png"))
        prefix = "icon.png";
      else if (name != NULL && g_str_has_prefix (name, "client-overrides/"))
        {
          prefix = "client-overrides/";
          if (archive_entry_filetype (header) == AE_IFREG)
            g_hash_table_add (client_overridden,
                              g_strdup (name + strlen ("client-overrides/")));
        }
      else if (name != NULL && g_str_has_prefix (name, "overrides/"))
        {
          const char *rel = name + strlen ("overrides/");
          if (archive_entry_filetype (header) != AE_IFREG ||
              !g_hash_table_contains (client_overridden, rel))
            prefix = "overrides/";
        }

      if (prefix != NULL &&
          !extract_entry (archive, header, directory, prefix, used, error))
        {
          archive_read_close (archive);
          close (fd);
          fd = -1;
          archive_read_free (archive);
          return FALSE;
        }

      if (archive_read_data_skip (archive) != ARCHIVE_OK)
        {
          archive_read_close (archive);
          close (fd);
          fd = -1;
          archive_read_free (archive);
          g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_ARCHIVE,
                       "the archive cannot be read all the way through");
          return FALSE;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, NUVASHALAUNCHER_MODPACK_ERROR, NUVASHALAUNCHER_MODPACK_ERROR_ARCHIVE,
                   "the overrides cannot be read: %s",
                   archive_error_string (archive));
      archive_read_close (archive);
      close (fd);
      fd = -1;
      archive_read_free (archive);
      return FALSE;
    }

  archive_read_close (archive);
  close (fd);
  fd = -1;
  archive_read_free (archive);

  return TRUE;
}

gboolean
nuvashalauncher_modpack_extract_overrides (const char *archive_path,
                                           const char *directory,
                                           GError **error)
{
  guint64 used = 0;

  return extract_layers (archive_path, directory, &used, error);
}

static gboolean
extract_overrides (const char *archive_path,
                   const char *directory,
                   GError **error)
{
  return nuvashalauncher_modpack_extract_overrides (archive_path, directory, error);
}

typedef struct {
  char *archive_path;
  char *directory;
  NuvashaLauncherTaskScope *scope;
} ExtractRun;

static void
extract_run_free (ExtractRun *run)
{
  if (run->scope != NULL)
    nuvashalauncher_task_scope_unref (run->scope);
  g_free (run->archive_path);
  g_free (run->directory);
  g_free (run);
}

static void
extract_thread (GTask *task G_GNUC_UNUSED,
                gpointer source_object G_GNUC_UNUSED,
                gpointer task_data, GCancellable *cancellable)
{
  ExtractRun *run = task_data;
  g_autoptr (NuvashaLauncherTaskScope) scope = g_steal_pointer (&run->scope);
  g_autoptr (GError) error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      nuvashalauncher_task_scope_return_error (
        scope,
        g_error_new_literal (NUVASHALAUNCHER_MODPACK_ERROR,
                             NUVASHALAUNCHER_MODPACK_ERROR_CANCELLED, "cancelled"));
      return;
    }

  if (!extract_overrides (run->archive_path, run->directory, &error))
    {
      nuvashalauncher_task_scope_return_error (scope, g_steal_pointer (&error));
      return;
    }

  if (g_cancellable_is_cancelled (cancellable))
    nuvashalauncher_task_scope_return_error (
      scope,
      g_error_new_literal (NUVASHALAUNCHER_MODPACK_ERROR,
                           NUVASHALAUNCHER_MODPACK_ERROR_CANCELLED, "cancelled"));
  else
    nuvashalauncher_task_scope_return_boolean (scope, TRUE);
}

NuvashaLauncherTaskScope *
extract_overrides_async (const char *archive_path, const char *directory,
                         GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvashalauncher_task_scope_task (scope);
  ExtractRun *run = g_new0 (ExtractRun, 1);

  /* Owned until extract_thread takes it. */
  run->scope = nuvashalauncher_task_scope_ref (scope);
  run->archive_path = g_strdup (archive_path);
  run->directory = g_strdup (directory);
  g_task_set_task_data (task, run, (GDestroyNotify) extract_run_free);
  g_task_run_in_thread (task, extract_thread);

  return g_steal_pointer (&scope);
}

gboolean
extract_overrides_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}
