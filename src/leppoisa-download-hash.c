/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Asynchronous hashing of an existing local file. Kept separate from the
 * HTTP transfer scheduler because cache validation uses the same operation
 * from several callers without involving a network request. */

#include "leppoisa-download-private.h"
#include "leppoisa-task-private.h"

#include <gio/gio.h>

#define HASH_CHUNK_SIZE (64 * 1024)

static void cache_open_ready (GObject *, GAsyncResult *, gpointer);
static void hash_read_ready (GObject *, GAsyncResult *, gpointer);

/* ---------------------------------------------------------- *
 * Hashing a file already on disk
 *
 * GIO has no checksum helper, so the file is read asynchronously in
 * chunks and fed to a GChecksum. The caller gets the hex digest, or
 * the error of the read; G_IO_ERROR_NOT_FOUND simply means that
 * nothing is cached yet.
 * ---------------------------------------------------------- */

typedef struct
{
  GTask *task;
  LeppoisaTaskScope *scope;
  GInputStream *stream;
  GChecksum *checksum;
} HashFile;

static void hash_read_next (HashFile *hash);

static void
hash_file_close (HashFile *hash)
{
  if (hash->stream != NULL)
    {
      g_input_stream_close (hash->stream, NULL, NULL);
      g_clear_object (&hash->stream);
    }
}

static void
hash_file_free (HashFile *hash)
{
  hash_file_close (hash);
  g_clear_pointer (&hash->checksum, g_checksum_free);
  g_clear_object (&hash->task);
  g_free (hash);
}

LeppoisaTaskScope *
leppoisa_download_hash_file_async (GFile *file, GChecksumType type,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (LeppoisaTaskScope) scope =
    leppoisa_task_scope_new (NULL, NULL, callback, user_data);
  HashFile *hash = g_new0 (HashFile, 1);

  /* Borrowed: the caller's handle keeps the operation alive, and the read
     is answered before it goes away. */
  hash->scope = scope;
  hash->task = g_object_ref (leppoisa_task_scope_task (scope));
  hash->checksum = g_checksum_new (type);

  g_file_read_async (file, G_PRIORITY_DEFAULT,
                     leppoisa_task_scope_get_cancellable (scope), cache_open_ready,
                     hash);

  return g_steal_pointer (&scope);
}

static void
cache_open_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  HashFile *hash = user_data;
  g_autoptr (GError) error = NULL;

  hash->stream = G_INPUT_STREAM (
    g_file_read_finish (G_FILE (source), result, &error));

  if (hash->stream == NULL)
    {
      leppoisa_task_scope_return_error (hash->scope, g_steal_pointer (&error));
      hash_file_free (hash);
      return;
    }

  hash_read_next (hash);
}

static void
hash_read_next (HashFile *hash)
{
  g_input_stream_read_bytes_async (hash->stream, HASH_CHUNK_SIZE,
                                   G_PRIORITY_DEFAULT,
                                    leppoisa_task_scope_get_cancellable (hash->scope),
                                   hash_read_ready, hash);
}

static void
hash_read_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  HashFile *hash = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (bytes == NULL)
    {
      leppoisa_task_scope_return_error (hash->scope, g_steal_pointer (&error));
      hash_file_free (hash);
      return;
    }

  gsize size = 0;
  const char *data = g_bytes_get_data (bytes, &size);

  if (size == 0)
    {
      leppoisa_task_scope_return_pointer (
        hash->scope, g_strdup (g_checksum_get_string (hash->checksum)), g_free);
      hash_file_free (hash);
      return;
    }

  g_checksum_update (hash->checksum, (const guchar *) data, size);
  hash_read_next (hash);
}

char *
leppoisa_download_hash_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

LeppoisaTaskScope *
leppoisa_download_file_hash_async (GFile *file, GChecksumType type,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  return leppoisa_download_hash_file_async (file, type, callback, user_data);
}

char *
leppoisa_download_file_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return leppoisa_download_hash_file_finish (result, error);
}
