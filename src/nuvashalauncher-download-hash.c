/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Asynchronous hashing of an existing local file. Kept separate from the
 * HTTP transfer scheduler because cache validation uses the same operation
 * from several callers without involving a network request. */

#include "nuvashalauncher-download-private.h"
#include "nuvashalauncher-task-private.h"

#include <gio/gio.h>

#define HASH_CHUNK_SIZE (256 * 1024)

/* ---------------------------------------------------------- *
 * Hashing a file already on disk
 *
 * GIO has no checksum helper, so the file is read on a worker thread
 * in chunks and fed to a GChecksum. The caller gets the hex digest,
 * or the error of the read; G_IO_ERROR_NOT_FOUND simply means that
 * nothing is cached yet.
 * ---------------------------------------------------------- */

typedef struct
{
  GFile *file;
  GChecksumType type;
  NuvashaLauncherTaskScope *scope;
} HashFile;

static void
hash_file_free (HashFile *hash)
{
  g_clear_object (&hash->file);
  g_clear_pointer (&hash->scope, nuvashalauncher_task_scope_unref);
  g_free (hash);
}

static void
hash_file_thread (GTask *task, gpointer source_object G_GNUC_UNUSED,
                  gpointer task_data, GCancellable *cancellable)
{
  HashFile *hash = task_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInputStream) stream = NULL;
  g_autoptr (GChecksum) checksum = NULL;
  g_autofree guchar *buffer = NULL;

  stream = g_file_read (hash->file, cancellable, &error);
  if (stream == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  checksum = g_checksum_new (hash->type);
  buffer = g_malloc (HASH_CHUNK_SIZE);

  for (;;)
    {
      gssize read_bytes = g_input_stream_read (G_INPUT_STREAM (stream), buffer,
                                               HASH_CHUNK_SIZE, cancellable,
                                               &error);

      if (read_bytes < 0)
        {
          g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }

      if (read_bytes == 0)
        break;

      g_checksum_update (checksum, buffer, (gsize) read_bytes);
    }

  g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);
  g_task_return_pointer (task, g_strdup (g_checksum_get_string (checksum)),
                         g_free);
}

static void
hash_file_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  HashFile *hash = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = g_task_propagate_pointer (G_TASK (result), &error);

  if (digest == NULL)
    nuvashalauncher_task_scope_return_error (hash->scope, g_steal_pointer (&error));
  else
    nuvashalauncher_task_scope_return_pointer (hash->scope,
                                               g_steal_pointer (&digest),
                                               g_free);

  hash_file_free (hash);
}

NuvashaLauncherTaskScope *
nuvashalauncher_download_hash_file_async (GFile *file, GChecksumType type,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autoptr (GTask) worker = NULL;
  HashFile *hash = g_new0 (HashFile, 1);

  hash->file = g_object_ref (file);
  hash->type = type;
  hash->scope = nuvashalauncher_task_scope_ref (scope);

  worker = g_task_new (NULL, nuvashalauncher_task_scope_get_cancellable (scope),
                       hash_file_done, hash);
  g_task_set_task_data (worker, hash, NULL);
  g_task_run_in_thread (worker, hash_file_thread);

  return g_steal_pointer (&scope);
}

char *
nuvashalauncher_download_hash_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

NuvashaLauncherTaskScope *
nuvashalauncher_download_file_hash_async (GFile *file, GChecksumType type,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  return nuvashalauncher_download_hash_file_async (file, type, callback, user_data);
}

char *
nuvashalauncher_download_file_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return nuvashalauncher_download_hash_file_finish (result, error);
}
