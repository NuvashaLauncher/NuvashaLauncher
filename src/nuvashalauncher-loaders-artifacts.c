/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Loader artifact queueing and transfer execution. */
#define _GNU_SOURCE

#include "nuvashalauncher-loaders-private.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-minecraft-resolve.h"

#include <json-glib/json-glib.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------- *
 * The files an install needs
 * ---------------------------------------------------------- */

void
queue_file (InstallRequest *request, InstallFileKind kind, const char *url,
            const char *entry, const char *path, const char *sha1,
            gboolean optional)
{
  InstallFile *file = g_new0 (InstallFile, 1);

  file->kind = kind;
  file->url = g_strdup (url);
  file->entry = g_strdup (entry);
  file->path = g_strdup (path);
  file->sha1 = g_strdup (sha1);
  file->optional = optional;

  g_ptr_array_add (request->pending, file);
}

/* Where the artifact @coordinate of the maven of the loader belongs. NULL
 * when the coordinates make no path, which the caller refuses. */
char *
artifact_path (InstallRequest *request, const char *coordinate)
{
  g_autofree char *relative = NULL;

  relative = nuvashalauncher_resolve_maven_path (coordinate);

  if (relative == NULL || !nuvashalauncher_resolve_entry_name_is_safe (relative))
    return NULL;

  return g_build_filename (request->libraries_dir, relative, NULL);
}

/* Queue the artifact named by @coordinate from the loader's Maven
 * repository. Existing files are reused; the repository is the source of
 * truth because loader metadata does not provide hashes.
 *
 * @optional marks processor outputs that may not exist in Maven. */
void
queue_artifact (InstallRequest *request, const char *coordinate,
                gboolean optional)
{
  const char *maven = NULL;
  const char *name = NULL;
  g_autofree char *relative = NULL;
  g_autofree char *path = NULL;
  g_autofree char *url = NULL;

  maven = loader_maven (request->loader, &name);

  if (maven == NULL)
    return;

  relative = nuvashalauncher_resolve_maven_path (coordinate);

  if (relative == NULL || !nuvashalauncher_resolve_entry_name_is_safe (relative))
    return;

  path = g_build_filename (request->libraries_dir, relative, NULL);

  {
    GStatBuf info;

    /* This pathname becomes a Java classpath entry. Do not treat a planted
     * symlink as an already fetched artifact. */
    if (g_lstat (path, &info) == 0 && S_ISREG (info.st_mode))
      {
        int fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0)
          {
            close (fd);
            return;
          }
      }
  }

  url = g_strconcat (maven, "/", relative, NULL);
  queue_file (request, FILE_FETCH, url, NULL, path, NULL, optional);
}

/* The file @entry of the installer jar, such as the patch the processors
 * apply, written out where the processors can find it. */
void
queue_unpacked (InstallRequest *request, const char *entry)
{
  g_autofree char *path = NULL;

  if (entry == NULL || *entry == '\0')
    return;

  /* The profile writes the paths inside its own jar with a leading
     slash, and this is what an entry has to look like to be one. */
  while (*entry == '/')
    entry++;

  if (*entry == '\0' || strstr (entry, "..") != NULL ||
      !nuvashalauncher_resolve_entry_name_is_safe (entry))
    return;

  path = g_build_filename (request->root, entry, NULL);

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return;

  queue_file (request, FILE_UNPACK, NULL, entry, path, NULL, FALSE);
}


#define LOADER_ARTIFACT_CONCURRENCY 6

typedef struct
{
  InstallRequest *request;
  GCancellable *outer_cancellable;
  GCancellable *children_cancellable;
  gulong cancel_handler;
  guint in_flight;
  GError *error;
} ArtifactBatch;

typedef struct
{
  ArtifactBatch *batch;
  InstallFile *file;
} ArtifactOp;

static void batch_dispatch (ArtifactBatch *batch);

static void
artifact_batch_free (ArtifactBatch *batch)
{
  if (batch == NULL)
    return;

  if (batch->cancel_handler != 0 && batch->outer_cancellable != NULL)
    g_cancellable_disconnect (batch->outer_cancellable, batch->cancel_handler);
  g_clear_object (&batch->outer_cancellable);
  g_clear_object (&batch->children_cancellable);
  g_clear_error (&batch->error);
  g_free (batch);
}

static void
on_batch_outer_cancelled (GCancellable *cancellable G_GNUC_UNUSED,
                          gpointer user_data)
{
  GCancellable *children = user_data;

  g_cancellable_cancel (children);
}

static void
file_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  ArtifactOp *op = user_data;
  ArtifactBatch *batch = op->batch;
  InstallRequest *request = batch->request;
  g_autoptr (GError) error = NULL;

  batch->in_flight--;

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      /* A maven that does not hold an artifact a name of the profile says
         nothing about: only the processors know, and they are run next. */
      if (op->file->optional &&
          g_error_matches (error, NUVASHALAUNCHER_DOWNLOAD_ERROR,
                           NUVASHALAUNCHER_DOWNLOAD_ERROR_HTTP))
        {
          g_clear_error (&error);
        }
      else if (batch->error == NULL)
        {
          batch->error = g_steal_pointer (&error);
          g_cancellable_cancel (batch->children_cancellable);
        }
    }

  g_free (op->file->url);
  g_free (op->file->entry);
  g_free (op->file->path);
  g_free (op->file->sha1);
  g_free (op->file);
  g_free (op);

  if (batch->error == NULL)
    {
      request->done++;
      report_progress (request);
      batch_dispatch (batch);
    }

  if (batch->in_flight == 0 &&
      (batch->error != NULL || request->pending->len == 0))
    {
      GError *err = g_steal_pointer (&batch->error);

      artifact_batch_free (batch);
      if (err != NULL)
        install_failed (request, err);
      else
        install_step_done (request);
    }
}

static void
batch_dispatch (ArtifactBatch *batch)
{
  InstallRequest *request = batch->request;

  while (batch->error == NULL &&
         request->pending->len > 0 &&
         batch->in_flight < LOADER_ARTIFACT_CONCURRENCY)
    {
      InstallFile *file = g_ptr_array_steal_index (request->pending,
                                                   request->pending->len - 1);
      g_autofree char *directory = g_path_get_dirname (file->path);
      g_autoptr (GError) error = NULL;

      if (!nuvashalauncher_loader_ensure_directory (directory, &error))
        {
          g_free (file->url);
          g_free (file->entry);
          g_free (file->path);
          g_free (file->sha1);
          g_free (file);
          batch->error = g_steal_pointer (&error);
          g_cancellable_cancel (batch->children_cancellable);
          break;
        }

      if (file->kind == FILE_UNPACK)
        {
          g_autoptr (GBytes) bytes =
            nuvashalauncher_loader_read_zip_entry (request->installer,
                                                   file->entry, &error);
          const char *data = NULL;
          gsize size = 0;
          gboolean ok = FALSE;

          if (bytes != NULL)
            {
              data = g_bytes_get_data (bytes, &size);
              ok = nuvashalauncher_loader_write_install_file (file->path, data,
                                                              size, &error);
            }

          g_free (file->url);
          g_free (file->entry);
          g_free (file->path);
          g_free (file->sha1);
          g_free (file);

          if (!ok)
            {
              batch->error = g_steal_pointer (&error);
              g_cancellable_cancel (batch->children_cancellable);
              break;
            }

          request->done++;
          report_progress (request);
          continue;
        }

      ArtifactOp *op = g_new0 (ArtifactOp, 1);
      op->batch = batch;
      op->file = file;
      batch->in_flight++;

      nuvashalauncher_task_scope_release_when_done (
        nuvashalauncher_download_file_async (
          nuvashalauncher_downloads_get_service (), file->url, file->path,
          file->sha1, file->sha1 != NULL ? G_CHECKSUM_SHA1 : G_CHECKSUM_MD5,
          NULL, file_ready, op),
        batch->children_cancellable);
    }
}

void
drain_pending (InstallRequest *request)
{
  ArtifactBatch *batch;

  if (request->pending->len == 0)
    {
      install_step_done (request);
      return;
    }

  batch = g_new0 (ArtifactBatch, 1);
  batch->request = request;
  batch->outer_cancellable = g_object_ref (g_task_get_cancellable (request->task));
  batch->children_cancellable = g_cancellable_new ();
  batch->cancel_handler = g_cancellable_connect (
    batch->outer_cancellable, G_CALLBACK (on_batch_outer_cancelled),
    g_object_ref (batch->children_cancellable), g_object_unref);

  batch_dispatch (batch);

  if (batch->in_flight == 0 &&
      (batch->error != NULL || request->pending->len == 0))
    {
      GError *err = g_steal_pointer (&batch->error);

      artifact_batch_free (batch);
      if (err != NULL)
        install_failed (request, err);
      else
        install_step_done (request);
    }
}
