#define _GNU_SOURCE

/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Modpack file verification and override extraction. Archive validation and
 * manifest parsing remain in aml-modpack-archive.c. */

#include "aml-modpack.h"
#include "aml-modpack-private.h"
#include "aml-download.h"
#include "aml-fs.h"
#include "aml-task-private.h"

#include <archive.h>
#include <fcntl.h>
#include <unistd.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------- *
 * Checking what arrived
 * ---------------------------------------------------------- */

typedef struct
{
  char *path;
  char *sha1;
  char *sha512;
  guint stage; /* 0: checking sha1, 1: checking sha512 */
  GTask *task; /* owned */
  AmlTaskScope *scope;
} VerifyRun;

static void
verify_run_free (VerifyRun *run)
{
  if (run == NULL)
    return;

  g_free (run->path);
  g_free (run->sha1);
  g_free (run->sha512);
  g_clear_object (&run->task);
  g_free (run);
}

static void
verify_hash_ready (GObject *source G_GNUC_UNUSED,
                   GAsyncResult *result,
                   gpointer user_data)
{
  VerifyRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = NULL;
  const char *expect = NULL;

  digest = aml_download_file_hash_finish (result, &error);

  if (digest == NULL)
    {
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      verify_run_free (run);
      return;
    }

  expect = run->stage == 0 ? run->sha1 : run->sha512;

  if (g_ascii_strcasecmp (expect, digest) != 0)
    {
      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
                   "\"%s\" arrived with a different %s than the index says",
                   run->path, run->stage == 0 ? "sha1" : "sha512");
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      verify_run_free (run);
      return;
    }

  run->stage++;

  if (run->stage > 1)
    {
      aml_task_scope_return_boolean (run->scope, TRUE);
      verify_run_free (run);
      return;
    }

  {
    g_autoptr (GFile) file = g_file_new_for_path (run->path);

    aml_task_scope_release_when_done (
      aml_download_file_hash_async (file, G_CHECKSUM_SHA512,
                                    verify_hash_ready, run),
      aml_task_scope_get_cancellable (run->scope));
  }
}

/* Both hashes of the manifest, out of the bytes on the disk. */
AmlTaskScope *
aml_modpack_verify_file_async (const AmlModpackFile *file,
                               const char *path,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  g_autoptr (GFile) job_file = g_file_new_for_path (path);
  VerifyRun *run = NULL;

  g_return_val_if_fail (file != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  if (file->sha1 == NULL || file->sha512 == NULL)
    {
      aml_task_scope_return_error_deferred (
        scope,
        g_error_new (AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
                     "\"%s\" does not carry the two hashes the format "
                     "requires", file->path != NULL ? file->path : "file"));
      return g_steal_pointer (&scope);
    }

  run = g_new0 (VerifyRun, 1);
  run->path = g_strdup (path);
  run->sha1 = g_strdup (file->sha1);
  run->sha512 = g_strdup (file->sha512);
  run->stage = 0;
  run->task = g_object_ref (task);

  /* Borrowed: the operation the caller owns until the answer. */
  run->scope = scope;

  aml_task_scope_release_when_done (
    aml_download_file_hash_async (job_file, G_CHECKSUM_SHA1, verify_hash_ready,
                                  run),
    aml_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

gboolean
aml_modpack_verify_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

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
      g_set_error_literal (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
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

  /* AML's content indexes are launcher metadata, never game files. */
  if (g_str_has_prefix (relative, ".index/") ||
      strstr (relative, "/.index/") != NULL)
    return TRUE;

  /* A directory ends in a /: no name of its own to check against the
   * safety rules, only its contents. */
  {
    gsize length = strlen (relative);

    if (length > 0 && relative[length - 1] == '/')
      relative[length - 1] = '\0';
  }

  if (!aml_modpack_path_is_safe (relative))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
                   "the overrides hold \"%s\", which is not a path the "
                   "instance can hold",
                   name);
      return FALSE;
    }

  if (archive_entry_filetype (header) == AE_IFDIR)
    {
      return aml_fs_make_directory_tree_nofollow (directory, relative, 0755,
                                                  error);
    }

  if (archive_entry_filetype (header) != AE_IFREG)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "\"%s\" is not a file", name);
      return FALSE;
    }

  {
    int fd = aml_fs_open_file_nofollow (directory, relative,
                                       O_WRONLY | O_CREAT | O_TRUNC, 0755, 0644,
                                       error);
    if (fd >= 0)
      out = fdopen (fd, "wb");
    if (out == NULL && fd >= 0)
      close (fd);
  }

  if (out == NULL)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "\"%s\" cannot be written: %s", name, g_strerror (errno));
      return FALSE;
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
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                       "\"%s\" cannot be read out of the archive: %s", name,
                       archive_error_string (archive));
          return FALSE;
        }

      if (size != 0)
        {
          /* The bomb protection runs on the bytes that did inflate, not
           * on what the entry announced. */
          if (*used > aml_modpack_get_content_limit () ||
              (guint64) size > aml_modpack_get_content_limit () - *used)
            {
              fclose (out);
              g_set_error_literal (error, AML_MODPACK_ERROR,
                                   AML_MODPACK_ERROR_BOMB,
                                   "the overrides decode past the size one "
                                   "pack carries");
              return FALSE;
            }

          if (fwrite (block, 1, size, out) != size)
            {
              fclose (out);
              g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                           "\"%s\" cannot be written: %s", name,
                           g_strerror (errno));
              return FALSE;
            }

          *used += size;
        }
    }

  if (fclose (out) != 0)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "\"%s\" cannot be closed: %s", name, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

/* One archive read for one of the two folder layers of a pack. */
static gboolean
extract_layers (const char *archive_path, const char *directory,
                guint64 *used, GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  int status = ARCHIVE_OK;
  guint paths = 0;

  archive = archive_read_new ();

  int fd = open (archive_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

  if (fd < 0 || archive_read_support_format_zip (archive) != ARCHIVE_OK ||
      archive_read_open_fd (archive, fd, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "the pack cannot be opened: %s",
                   archive_error_string (archive));
      archive_read_free (archive);
      return FALSE;
    }

  while ((status = archive_read_next_header (archive, &header)) == ARCHIVE_OK)
    {
      if (++paths > AML_MODPACK_MAX_ENTRIES)
        {
          g_set_error_literal (error, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_BOMB,
                               "the archive has more entries than one pack");
          archive_read_close (archive);
          archive_read_free (archive);
          return FALSE;
        }

      if (!aml_modpack_archive_entry_is_safe (
            archive_entry_pathname (header), archive_entry_filetype (header),
            archive_entry_hardlink (header), error))
        {
          archive_read_close (archive);
          archive_read_free (archive);
          return FALSE;
        }

      const char *name = archive_entry_pathname (header);
      const char *prefix = NULL;

      if (name != NULL && g_str_equal (name, "icon.png"))
        prefix = "icon.png";
      else if (name != NULL && g_str_has_prefix (name, "client-overrides/"))
        prefix = "client-overrides/";
      else if (name != NULL && g_str_has_prefix (name, "overrides/"))
        prefix = "overrides/";

      if (prefix != NULL &&
          !extract_entry (archive, header, directory, prefix, used, error))
        {
          archive_read_close (archive);
          archive_read_free (archive);
          return FALSE;
        }

      if (archive_read_data_skip (archive) != ARCHIVE_OK)
        {
          archive_read_close (archive);
          archive_read_free (archive);
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                       "the archive cannot be read all the way through");
          return FALSE;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "the overrides cannot be read: %s",
                   archive_error_string (archive));
      archive_read_close (archive);
      archive_read_free (archive);
      return FALSE;
    }

  archive_read_close (archive);
  archive_read_free (archive);

  return TRUE;
}

/* client-overrides wins over overrides, so it lands on top of what
 * overrides wrote; server-overrides has nothing to do on the client. */
static gboolean
extract_overrides (const char *archive_path,
                   const char *directory,
                   GError **error)
{
  guint64 used = 0;

  return extract_layers (archive_path, directory, &used, error);
}

typedef struct {
  char *archive_path;
  char *directory;
  AmlTaskScope *scope;
} ExtractRun;

static void
extract_run_free (ExtractRun *run)
{
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
  g_autoptr (GError) error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      aml_task_scope_return_error (
        run->scope,
        g_error_new_literal (AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_CANCELLED, "cancelled"));
      return;
    }

  if (!extract_overrides (run->archive_path, run->directory, &error))
    {
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      return;
    }

  if (g_cancellable_is_cancelled (cancellable))
    aml_task_scope_return_error (
      run->scope,
      g_error_new_literal (AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_CANCELLED, "cancelled"));
  else
    aml_task_scope_return_boolean (run->scope, TRUE);
}

AmlTaskScope *
extract_overrides_async (const char *archive_path, const char *directory,
                         GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  ExtractRun *run = g_new0 (ExtractRun, 1);

  /* Borrowed: the extract is the task's own data and the scope owns that
     task. The caller keeps the operation alive until its callback. */
  run->scope = scope;
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
