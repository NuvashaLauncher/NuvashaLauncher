/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-cache.h"
#include "nuvasha-fs.h"

#define CACHE_SUBDIR "nuvasha"

static char *
cache_directory (const char *child)
{
  return g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, child, NULL);
}

char *
nuvasha_cache_get_api_directory (void)
{
  return cache_directory ("api");
}

char *
nuvasha_cache_get_files_directory (void)
{
  return cache_directory ("files");
}

gboolean
nuvasha_cache_ensure_directory (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return nuvasha_fs_make_directory (path, 0755, error);
}

static NuvashaTaskScope *
cache_clear_async (const char *path, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  return nuvasha_fs_remove_tree_async (path, callback, user_data);
}

NuvashaTaskScope *
nuvasha_cache_clear_api_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path = nuvasha_cache_get_api_directory ();
  return cache_clear_async (path, callback, user_data);
}

NuvashaTaskScope *
nuvasha_cache_clear_all_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, NULL);
  return cache_clear_async (path, callback, user_data);
}
