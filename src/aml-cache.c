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

static void
cache_clear_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  AmlTaskScope *scope = user_data;
  g_autoptr (GError) error = NULL;

  if (aml_fs_remove_tree_finish (result, &error))
    aml_task_scope_return_boolean (scope, TRUE);
  else
    aml_task_scope_return_error (scope, g_steal_pointer (&error));
}

static AmlTaskScope *
cache_clear_async (const char *path, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  AmlTaskScope *scope = aml_task_scope_new (NULL, NULL, callback, user_data);
  aml_fs_remove_tree_async (path, aml_task_scope_get_cancellable (scope),
                            cache_clear_done, scope);
  return scope;
}

AmlTaskScope *
aml_cache_clear_api_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path = aml_cache_get_api_directory ();
  return cache_clear_async (path, callback, user_data);
}

AmlTaskScope *
aml_cache_clear_all_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, NULL);
  return cache_clear_async (path, callback, user_data);
}
