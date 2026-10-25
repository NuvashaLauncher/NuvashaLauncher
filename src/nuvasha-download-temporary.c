/* Download temporary-file management. */

#include "nuvasha-download-private.h"
#include "nuvasha-fs.h"

#include <unistd.h>
#include <gio/gunixoutputstream.h>

void
nuvasha_download_close_sink (DownloadRequest *request)
{
  if (request->sink != NULL)
    {
      g_output_stream_close (request->sink, NULL, NULL);
      g_clear_object (&request->sink);
    }

  request->fd = -1;
}

void
nuvasha_download_discard_temporary (DownloadRequest *request)
{
  if (request->tmp_path != NULL)
    {
      if (request->parent_fd >= 0 && request->tmp_name != NULL)
        nuvasha_fs_unlink_at (request->parent_fd, request->tmp_name, NULL);
      else
        nuvasha_fs_unlink (request->tmp_path, NULL);
      g_clear_pointer (&request->tmp_path, g_free);
    }
  g_clear_pointer (&request->tmp_name, g_free);
  if (request->parent_fd >= 0)
    {
      close (request->parent_fd);
      request->parent_fd = -1;
    }

  nuvasha_download_close_sink (request);
}

gboolean
nuvasha_download_open_temporary (DownloadRequest *request, GError **error)
{
  g_autofree char *directory = g_path_get_dirname (request->dest_path);

  /* Do not create a temporary beside a destination reached through a
   * symlink. The download is later published with rename(), so accepting a
   * substituted parent would write into a directory other than the one the
   * caller selected. The instance installer creates real private temporary
   * directories, and rejecting symlink parents is the safe default for all
   * other callers too. */
  request->fd = nuvasha_fs_open_temporary_file_nofollow (
    directory, &request->tmp_name, &request->tmp_path, &request->parent_fd,
    error);
  if (request->fd < 0)
    return FALSE;
  request->sink = g_unix_output_stream_new (request->fd, TRUE);

  return TRUE;
}
