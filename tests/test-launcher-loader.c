/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-launcher-loader-private.h"
#include "nuvashalauncher-launcher-private.h"
#include "nuvashalauncher-task-private.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>

static char *root;

static nuvashalauncherInstance *
create_loader_instance (const char *name)
{
  g_autofree char *dir = g_build_filename (root, name, NULL);
  g_autoptr (GError) error = NULL;
  nuvashalauncherInstance *instance = NULL;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  instance = nuvashalauncher_instance_new (NULL, name, dir, "1.20.1",
                                           nuvashalauncher_LOADER_FABRIC, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  return instance;
}

static void
test_loader_request_lifecycle (void)
{
  g_autoptr (nuvashalauncherInstance) instance = create_loader_instance ("req-inst");
  g_autoptr (nuvashalauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);

  LoaderRequest *request = g_new0 (LoaderRequest, 1);
  request->launcher = g_object_ref (launcher);
  request->loader = nuvashalauncher_LOADER_FABRIC;
  request->game_version = g_strdup ("1.20.1");
  request->loader_version = g_strdup ("0.15.0");
  request->operation = nuvashalauncher_task_scope_new_tagged (
    NULL, NULL, NULL, NULL, "loader-test-scope");

  g_assert_nonnull (request->launcher);
  g_assert_nonnull (request->operation);

  /* loader_request_free cleans up scopes, profiles, strings, and launcher references */
  loader_request_free (request);
}

static void
test_java_for_install_failure_handling (void)
{
  g_autoptr (nuvashalauncherInstance) instance = create_loader_instance ("fail-inst");
  g_autoptr (nuvashalauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);

  LoaderRequest *request = g_new0 (LoaderRequest, 1);
  request->launcher = g_object_ref (launcher);
  request->loader = nuvashalauncher_LOADER_FABRIC;
  request->game_version = g_strdup ("1.20.1");
  request->loader_version = g_strdup ("0.15.0");

  /* Simulate failed Java detection result */
  nuvashalauncherTaskScope *scope = nuvashalauncher_task_scope_new_tagged (
    NULL, NULL, NULL, NULL, "nuvashalauncher-task-scope");
  nuvashalauncher_task_scope_return_error (
    scope, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no java found"));

  on_java_for_the_install (NULL, G_ASYNC_RESULT (nuvashalauncher_task_scope_task (scope)), request);
  nuvashalauncher_task_scope_unref (scope);

  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_FAILED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  root = g_dir_make_tmp ("nuvashalauncher-loader-test-XXXXXX", NULL);

  g_test_add_func ("/launcher/loader/request-lifecycle", test_loader_request_lifecycle);
  g_test_add_func ("/launcher/loader/java-failure", test_java_for_install_failure_handling);

  int ret = g_test_run ();

  g_rmdir (root);
  g_free (root);
  return ret;
}
