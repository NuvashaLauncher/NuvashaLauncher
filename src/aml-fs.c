/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-fs.h"

#include <errno.h>
#include <glib/gstdio.h>

gboolean
aml_fs_replace_contents (const char *path, const void *contents,
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

  return TRUE;
}

gboolean
aml_fs_make_private_directory (const char *path, GError **error)
{
  if (g_mkdir_with_parents (path, 0700) != 0 || g_chmod (path, 0700) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot secure \"%s\": %s", path, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}
