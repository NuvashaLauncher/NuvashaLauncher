/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-launcher-private.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-task-private.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-instance.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-minecraft-versions.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>

static char *root;

static NuvashaLauncherInstance *
create_test_instance (const char *name)
{
  g_autofree char *dir = g_build_filename (root, name, NULL);
  g_autoptr (GError) error = NULL;
  NuvashaLauncherInstance *instance = NULL;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  instance = nuvashalauncher_instance_new (NULL, name, dir, "1.20.1",
                                           nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  return instance;
}

static void
test_choose_java_compatible_and_headless (void)
{
  g_autoptr (NuvashaLauncherInstance) instance = create_test_instance ("java-inst");
  g_autoptr (NuvashaLauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) javas = g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_java_free);
  g_autofree char *inst_java = NULL;
  const NuvashaLauncherJava *chosen = NULL;

  /* No Javas available */
  chosen = choose_java (launcher, javas, 17, &inst_java);
  g_assert_null (chosen);
  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_FAILED);

  /* Add headless Java runtime */
  NuvashaLauncherJava *headless = g_new0 (NuvashaLauncherJava, 1);
  headless->executable = g_strdup ("/usr/bin/fake-headless-java");
  headless->major_version = 17;
  headless->headless = TRUE;
  g_ptr_array_add (javas, headless);

  chosen = choose_java (launcher, javas, 17, &inst_java);
  g_assert_null (chosen);
  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_FAILED);

  /* Add compatible desktop Java */
  NuvashaLauncherJava *desktop = g_new0 (NuvashaLauncherJava, 1);
  desktop->executable = g_strdup ("/usr/bin/fake-desktop-java");
  desktop->major_version = 17;
  desktop->headless = FALSE;
  g_ptr_array_add (javas, desktop);

  chosen = choose_java (launcher, javas, 17, &inst_java);
  g_assert_nonnull (chosen);
  g_assert_cmpstr (chosen->executable, ==, "/usr/bin/fake-desktop-java");

  /* Test remember_java */
  remember_java (launcher, chosen);
  g_assert_cmpstr (launcher->preparation.java_executable, ==, "/usr/bin/fake-desktop-java");
  g_assert_cmpint (launcher->preparation.java_major_version, ==, 17);
}

static void
test_resolve_progress_tracking (void)
{
  g_autoptr (NuvashaLauncherInstance) instance = create_test_instance ("progress-inst");
  g_autoptr (NuvashaLauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);
  NuvashaLauncherResolveProgress prog = { 0 };

  prog.stage = "Downloading assets";
  prog.done = 50;
  prog.total = 100;

  on_resolve_progress (&prog, launcher);
  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_DOWNLOADING);
  g_assert_cmpuint (launcher->process.last_percent, ==, 50);

  /* Identical percentage should be debounced and ignored */
  prog.done = 50;
  on_resolve_progress (&prog, launcher);
  g_assert_cmpuint (launcher->process.last_percent, ==, 50);

  /* Completion transitions out of downloading to preparing */
  prog.done = 100;
  prog.total = 100;
  on_resolve_progress (&prog, launcher);
  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_PREPARING);

  /* Stop requested stops processing */
  launcher->process.stop_requested = TRUE;
  prog.done = 10;
  prog.total = 100;
  on_resolve_progress (&prog, launcher);
  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_PREPARING);
}

static void
test_on_resolved_lifecycle (void)
{
  g_autoptr (NuvashaLauncherInstance) instance = create_test_instance ("resolved-inst");
  g_autoptr (NuvashaLauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);

  /* Create a task scope tagged "nuvashalauncher-task-scope" */
  NuvashaLauncherTaskScope *scope = nuvashalauncher_task_scope_new_tagged (
    NULL, NULL, NULL, NULL, "nuvashalauncher-task-scope");
  nuvashalauncher_task_scope_return_error (
    scope, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "failed resolve"));

  g_object_ref (launcher);
  on_resolved (NULL, G_ASYNC_RESULT (nuvashalauncher_task_scope_task (scope)), launcher);
  nuvashalauncher_task_scope_unref (scope);

  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_FAILED);
}

static void
test_on_resolved_success_triggers_java_search (void)
{
  g_autoptr (NuvashaLauncherInstance) instance = create_test_instance ("resolved-succ-inst");
  g_autoptr (NuvashaLauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);

  NuvashaLauncherResolvedVersion *res = g_new0 (NuvashaLauncherResolvedVersion, 1);
  res->version_id = g_strdup ("1.20.1");
  res->java_major_version = 17;

  NuvashaLauncherTaskScope *scope = nuvashalauncher_task_scope_new_tagged (
    NULL, NULL, NULL, NULL, "nuvashalauncher-task-scope");
  nuvashalauncher_task_scope_return_pointer (scope, res, (GDestroyNotify) nuvashalauncher_resolved_version_free);

  g_object_ref (launcher);
  on_resolved (NULL, G_ASYNC_RESULT (nuvashalauncher_task_scope_task (scope)), launcher);
  nuvashalauncher_task_scope_unref (scope);

  /* Should set state to launching and detail to choosing Java */
  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_LAUNCHING);
}

static void
test_on_resolved_with_preset_java_runs_game (void)
{
  g_autoptr (NuvashaLauncherInstance) instance = create_test_instance ("resolved-preset-inst");
  g_autoptr (NuvashaLauncher) launcher = nuvashalauncher_launcher_new (instance, NULL, NULL);

  /* Set up a preset java executable so on_resolved proceeds straight to run_the_game */
  launcher->preparation.java_executable = g_strdup ("/usr/bin/fake-java-exec");
  launcher->preparation.java_major_version = 17;

  NuvashaLauncherResolvedVersion *res = g_new0 (NuvashaLauncherResolvedVersion, 1);
  res->version_id = g_strdup ("1.20.1");
  res->main_class = g_strdup ("net.minecraft.client.main.Main");
  res->classpath = g_strdup ("/path/to/client.jar");
  res->java_major_version = 17;

  NuvashaLauncherTaskScope *scope = nuvashalauncher_task_scope_new_tagged (
    NULL, NULL, NULL, NULL, "nuvashalauncher-task-scope");
  nuvashalauncher_task_scope_return_pointer (scope, res, (GDestroyNotify) nuvashalauncher_resolved_version_free);

  g_object_ref (launcher);
  on_resolved (NULL, G_ASYNC_RESULT (nuvashalauncher_task_scope_task (scope)), launcher);
  nuvashalauncher_task_scope_unref (scope);

  /* Fake java executable won't spawn successfully, so state becomes failed */
  g_assert_cmpint (nuvashalauncher_launcher_get_state (launcher), ==, nuvashalauncher_LAUNCH_FAILED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  root = g_dir_make_tmp ("nuvashalauncher-prepare-test-XXXXXX", NULL);

  g_test_add_func ("/launcher/prepare/choose-java", test_choose_java_compatible_and_headless);
  g_test_add_func ("/launcher/prepare/progress", test_resolve_progress_tracking);
  g_test_add_func ("/launcher/prepare/resolved-lifecycle", test_on_resolved_lifecycle);
  g_test_add_func ("/launcher/prepare/resolved-success", test_on_resolved_success_triggers_java_search);
  g_test_add_func ("/launcher/prepare/resolved-preset-java", test_on_resolved_with_preset_java_runs_game);

  int ret = g_test_run ();

  g_rmdir (root);
  g_free (root);
  return ret;
}
