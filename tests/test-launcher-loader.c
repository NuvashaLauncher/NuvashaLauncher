/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-launcher-private.h"
#include "nuvashalauncher-task-private.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>

static char *root;

static NuvashaLauncherInstance *
create_loader_instance (const char *name)
{
  g_autofree char *dir = g_build_filename (root, name, NULL);
  g_autoptr (GError) error = NULL;
  NuvashaLauncherInstance *instance = NULL;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  instance = nuvashalauncher_instance_new (NULL, name, dir, "1.20.1",
                                           NUVASHALAUNCHER_LOADER_FABRIC, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  return instance;
}

static void
test_manifest_ready_error_handling (void)
{
  g_autoptr (NuvashaLauncherInstance) instance = create_loader_instance ("manifest-fail");
  g_autoptr (NuvashaLauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);

  NuvashaLauncherTaskScope *scope = nuvashalauncher_task_scope_new_tagged (
    NULL, NULL, NULL, NULL, "nuvashalauncher-task-scope");
  nuvashalauncher_task_scope_return_error (
    scope, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "failed to get manifest"));

  on_manifest_ready (NULL, G_ASYNC_RESULT (nuvashalauncher_task_scope_task (scope)), g_object_ref (launcher));
  nuvashalauncher_task_scope_unref (scope);

  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, NUVASHALAUNCHER_LAUNCH_FAILED);
}

static void
test_manifest_ready_unsupported_loader (void)
{
  /* Instance with unsupported loader or empty loader-version */
  g_autofree char *dir = g_build_filename (root, "unsupported-loader-inst", NULL);
  g_autoptr (GError) error = NULL;
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);

  /* Create instance with Fabric but no loader-version */
  g_autoptr (NuvashaLauncherInstance) instance =
    nuvashalauncher_instance_new (NULL, "unsupported-loader-inst", dir, "1.20.1",
                                  NUVASHALAUNCHER_LOADER_FABRIC, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autoptr (NuvashaLauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);

  g_autoptr (NuvashaLauncherVersionManifest) manifest =
    g_object_new (NUVASHALAUNCHER_TYPE_VERSION_MANIFEST, NULL);

  NuvashaLauncherTaskScope *scope = nuvashalauncher_task_scope_new_tagged (
    NULL, NULL, NULL, NULL, "nuvashalauncher-task-scope");
  nuvashalauncher_task_scope_return_pointer (scope, g_object_ref (manifest), g_object_unref);

  on_manifest_ready (NULL, G_ASYNC_RESULT (nuvashalauncher_task_scope_task (scope)), g_object_ref (launcher));
  nuvashalauncher_task_scope_unref (scope);

  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, NUVASHALAUNCHER_LAUNCH_FAILED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  root = g_dir_make_tmp ("nuvashalauncher-loader-test-XXXXXX", NULL);

  g_test_add_func ("/launcher/loader/manifest-error", test_manifest_ready_error_handling);
  g_test_add_func ("/launcher/loader/unsupported-loader", test_manifest_ready_unsupported_loader);

  int ret = g_test_run ();

  g_rmdir (root);
  g_free (root);
  return ret;
}
