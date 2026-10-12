/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-cache.h"
#include "aml-fs.h"

#define CACHE_SUBDIR "AML"

static char *
cache_directory (const char *child)
{
  return g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, child, NULL);
}

char *
aml_cache_get_api_directory (void)
{
  return cache_directory ("api");
}

char *
aml_cache_get_files_directory (void)
{
  return cache_directory ("files");
}

void
aml_cache_clear_api_async (GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path = aml_cache_get_api_directory ();
  aml_fs_remove_tree_async (path, cancellable, callback, user_data);
}

void
aml_cache_clear_all_async (GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, NULL);
  aml_fs_remove_tree_async (path, cancellable, callback, user_data);
}
