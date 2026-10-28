/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-instance-manager.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

static char *data_home;

static char *
run_cli (const char *first, const char *second, int *status)
{
  const char *argv[] = { nuvashalauncher_BIN, first, second, NULL };
  gchar **env = g_get_environ ();
  char *output = NULL;
  char *errors = NULL;
  g_autoptr (GError) error = NULL;

  env = g_environ_setenv (env, "XDG_DATA_HOME", data_home, TRUE);
  env = g_environ_setenv (env, "GSETTINGS_BACKEND", "memory", TRUE);
  g_assert_true (g_spawn_sync (NULL, (char **) argv, env, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, &output, &errors, status, &error));
  g_assert_no_error (error);
  g_strfreev (env);
  g_free (errors);
  return output;
}

static void
test_account_commands (void)
{
  g_autofree char *accounts_path =
    g_build_filename (data_home, "nuvashalauncher", "accounts.json", NULL);
  g_autoptr (nuvashalauncherAccounts) accounts =
    nuvashalauncher_accounts_new_for_path (accounts_path);
  g_autoptr (nuvashalauncherAccount) first = NULL;
  g_autoptr (nuvashalauncherAccount) second = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *output = NULL;
  g_autofree char *accounts_dir = NULL;
  int status = -1;

  accounts_dir = g_path_get_dirname (accounts_path);
  g_assert_cmpint (g_mkdir_with_parents (accounts_dir, 0700), ==, 0);

  first = nuvashalauncher_account_new_offline ("Alex", &error);
  g_assert_no_error (error);
  second = nuvashalauncher_account_new_offline ("Steve", &error);
  g_assert_no_error (error);
  if (!nuvashalauncher_accounts_add (accounts, first, &error))
    {
      g_test_skip ("Secret Service is unavailable in this environment");
      return;
    }
  g_assert_no_error (error);
  g_assert_true (nuvashalauncher_accounts_add (accounts, second, &error));
  g_assert_no_error (error);

  output = run_cli ("--list-accounts", NULL, &status);
  g_assert_cmpint (status, ==, 0);
  g_assert_nonnull (strstr (output, first->uuid));
  g_assert_nonnull (strstr (output, "Alex"));
  g_assert_nonnull (strstr (output, second->uuid));
  g_assert_nonnull (strstr (output, "Steve"));
  g_clear_pointer (&output, g_free);

  output = run_cli ("--switch-account", "Alex", &status);
  g_assert_cmpint (status, ==, 0);
  g_clear_pointer (&output, g_free);
  g_clear_object (&accounts);
  accounts = nuvashalauncher_accounts_new_for_path (accounts_path);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active_id (accounts), ==, first->id);

  output = run_cli ("--remove-account", second->uuid, &status);
  g_assert_cmpint (status, ==, 0);
  g_clear_pointer (&output, g_free);
  g_clear_object (&accounts);
  accounts = nuvashalauncher_accounts_new_for_path (accounts_path);
  g_assert_cmpuint (nuvashalauncher_accounts_count (accounts), ==, 1);
  g_assert_nonnull (nuvashalauncher_accounts_find (accounts, first->id));

  output = run_cli ("--remove-account", "does-not-exist", &status);
  g_assert_cmpint (status, !=, 0);
  g_clear_pointer (&output, g_free);
}

static void
test_remove_instance_command (void)
{
  g_autofree char *manager_path =
    g_build_filename (data_home, "nuvashalauncher", NULL);
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (manager_path);
  g_autoptr (nuvashalauncherInstanceManager) reloaded = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *output = NULL;
  g_autofree char *directory = NULL;
  int status = -1;

  g_assert_true (nuvashalauncher_instance_manager_load (manager, &error));
  g_assert_no_error (error);
  instance = nuvashalauncher_instance_manager_create (manager, "CLI Test Instance", NULL,
                                               "1.20.1", nuvashalauncher_LOADER_VANILLA,
                                               NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_object_get (instance, "directory", &directory, NULL);

  output = run_cli ("--remove-instance", nuvashalauncher_instance_get_id (instance), &status);
  g_assert_cmpint (status, ==, 0);
  g_clear_pointer (&output, g_free);

  reloaded = nuvashalauncher_instance_manager_new (manager_path);
  g_assert_true (nuvashalauncher_instance_manager_load (reloaded, &error));
  g_assert_no_error (error);
  g_assert_null (nuvashalauncher_instance_manager_get_by_id (
    reloaded, nuvashalauncher_instance_get_id (instance)));
  g_assert_false (g_file_test (directory, G_FILE_TEST_EXISTS));

  g_print ("ok - the instance CLI command removes metadata and files\n");
}

int
main (void)
{
  data_home = g_dir_make_tmp ("nuvashalauncher-cli-XXXXXX", NULL);
  if (data_home == NULL)
    g_error ("could not make a temporary data directory");
  g_setenv ("XDG_DATA_HOME", data_home, TRUE);

  test_account_commands ();
  test_remove_instance_command ();
  g_free (data_home);
  g_print ("ok - account CLI commands list, switch and remove accounts\n");
  return EXIT_SUCCESS;
}
