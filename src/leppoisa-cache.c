/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-cache.h"
#include "leppoisa-fs.h"

#define CACHE_SUBDIR "leppoisa"

static char *
cache_directory (const char *child)
{
  return g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, child, NULL);
}

char *
leppoisa_cache_get_api_directory (void)
{
  return cache_directory ("api");
}

char *
leppoisa_cache_get_files_directory (void)
{
  return cache_directory ("files");
}

gboolean
leppoisa_cache_ensure_directory (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return leppoisa_fs_make_directory (path, 0755, error);
}

static LeppoisaTaskScope *
cache_clear_async (const char *path, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  return leppoisa_fs_remove_tree_async (path, callback, user_data);
}

LeppoisaTaskScope *
leppoisa_cache_clear_api_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path = leppoisa_cache_get_api_directory ();
  return cache_clear_async (path, callback, user_data);
}

LeppoisaTaskScope *
leppoisa_cache_clear_all_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, NULL);
  return cache_clear_async (path, callback, user_data);
}
