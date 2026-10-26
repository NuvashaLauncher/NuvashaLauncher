/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-fs.h"

#define CACHE_SUBDIR "nuvashalauncher"

static char *
cache_directory (const char *child)
{
  return g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, child, NULL);
}

char *
nuvashalauncher_cache_get_api_directory (void)
{
  return cache_directory ("api");
}

char *
nuvashalauncher_cache_get_files_directory (void)
{
  return cache_directory ("files");
}

gboolean
nuvashalauncher_cache_ensure_directory (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return nuvashalauncher_fs_make_directory (path, 0755, error);
}

static nuvashalauncherTaskScope *
cache_clear_async (const char *path, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  return nuvashalauncher_fs_remove_tree_async (path, callback, user_data);
}

nuvashalauncherTaskScope *
nuvashalauncher_cache_clear_api_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path = nuvashalauncher_cache_get_api_directory ();
  return cache_clear_async (path, callback, user_data);
}

nuvashalauncherTaskScope *
nuvashalauncher_cache_clear_all_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, NULL);
  return cache_clear_async (path, callback, user_data);
}
