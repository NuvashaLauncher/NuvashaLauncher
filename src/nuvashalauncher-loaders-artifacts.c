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


static void
file_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  InstallRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  InstallFile *file = g_ptr_array_index (request->pending, 0);

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      /* A maven that does not hold an artifact a name of the profile says
         nothing about: only the processors know, and they are run next. */
      if (file->optional && g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                                             nuvashalauncher_DOWNLOAD_ERROR_HTTP))
        g_clear_error (&error);
      else
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }
    }

  g_ptr_array_remove_index (request->pending, 0);
  request->done++;

  report_progress (request);
  drain_pending (request);
}

void
drain_pending (InstallRequest *request)
{
  InstallFile *file = NULL;
  g_autofree char *directory = NULL;
  g_autoptr (GError) error = NULL;

  if (request->pending->len == 0)
    {
      install_step_done (request);
      return;
    }

  file = g_ptr_array_index (request->pending, 0);
  directory = g_path_get_dirname (file->path);

  if (!nuvashalauncher_loader_ensure_directory (directory, &error))
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  if (file->kind == FILE_UNPACK)
    {
      g_autoptr (GBytes) bytes =
        nuvashalauncher_loader_read_zip_entry (request->installer, file->entry, &error);
      const char *data = NULL;
      gsize size = 0;

      if (bytes == NULL)
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }

      data = g_bytes_get_data (bytes, &size);

      if (!nuvashalauncher_loader_write_install_file (file->path, data, size, &error))
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }

      g_ptr_array_remove_index (request->pending, 0);
      request->done++;

      report_progress (request);
      drain_pending (request);
      return;
    }

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), file->url, file->path, file->sha1,
                             file->sha1 != NULL ? G_CHECKSUM_SHA1
                                                : G_CHECKSUM_MD5,
                             NULL, file_ready, request),
    g_task_get_cancellable (request->task));
}
