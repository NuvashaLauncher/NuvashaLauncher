/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-nbt.h"
#include "nuvashalauncher-secret-store.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static char *data_home;

static char *
run_cli_v (const char * const *args, int *status)
{
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  gchar **env = g_get_environ ();
  char *output = NULL;
  char *errors = NULL;
  g_autoptr (GError) error = NULL;

  g_ptr_array_add (argv, (gpointer) NUVASHALAUNCHER_BIN);
  for (guint i = 0; args[i] != NULL; i++)
    g_ptr_array_add (argv, (gpointer) args[i]);
  g_ptr_array_add (argv, NULL);

  env = g_environ_setenv (env, "XDG_DATA_HOME", data_home, TRUE);
  env = g_environ_setenv (env, "GSETTINGS_BACKEND", "memory", TRUE);
  env = g_environ_unsetenv (env, "DISPLAY");
  env = g_environ_unsetenv (env, "WAYLAND_DISPLAY");
  g_assert_true (g_spawn_sync (NULL, (char **) argv->pdata, env, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, &output, &errors, status, &error));
  g_assert_no_error (error);
  g_strfreev (env);
  g_free (errors);
  return output;
}

static char *
run_cli (const char *first, const char *second, int *status)
{
  const char *args[] = { first, second, NULL };
  return run_cli_v (args, status);
}

typedef struct
{
  const char * const *args;
  char *stdout_buf;
  int exit_status;
  gboolean exited;
  int *remaining;
} CliBatchItem;

static void
on_cli_batch_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  CliBatchItem *item = user_data;
  GSubprocess *proc = G_SUBPROCESS (source);
  g_autoptr (GError) error = NULL;
  g_autofree char *stderr_buf = NULL;

  g_assert_true (g_subprocess_communicate_utf8_finish (proc, res,
                                                      &item->stdout_buf,
                                                      &stderr_buf, &error));
  g_assert_no_error (error);
  item->exited = g_subprocess_get_if_exited (proc);
  item->exit_status = item->exited ? g_subprocess_get_exit_status (proc) : -1;
  (*item->remaining)--;
}

static void
run_cli_batch (CliBatchItem *items, size_t count)
{
  g_autoptr (GSubprocessLauncher) launcher =
    g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                               G_SUBPROCESS_FLAGS_STDERR_PIPE);
  int remaining = (int) count;

  g_subprocess_launcher_setenv (launcher, "XDG_DATA_HOME", data_home, TRUE);
  g_subprocess_launcher_setenv (launcher, "GSETTINGS_BACKEND", "memory", TRUE);
  g_subprocess_launcher_unsetenv (launcher, "DISPLAY");
  g_subprocess_launcher_unsetenv (launcher, "WAYLAND_DISPLAY");

  for (size_t i = 0; i < count; i++)
    {
      g_autoptr (GPtrArray) argv = g_ptr_array_new ();
      g_autoptr (GError) error = NULL;
      g_autoptr (GSubprocess) proc = NULL;

      g_ptr_array_add (argv, (gpointer) NUVASHALAUNCHER_BIN);
      for (guint j = 0; items[i].args[j] != NULL; j++)
        g_ptr_array_add (argv, (gpointer) items[i].args[j]);
      g_ptr_array_add (argv, NULL);

      items[i].remaining = &remaining;
      proc = g_subprocess_launcher_spawnv (launcher,
                                           (const char * const *) argv->pdata,
                                           &error);
      g_assert_no_error (error);
      g_assert_nonnull (proc);
      g_subprocess_communicate_utf8_async (proc, NULL, NULL,
                                           on_cli_batch_done, &items[i]);
    }

  while (remaining > 0)
    g_main_context_iteration (NULL, TRUE);
}

static void
test_account_commands (void)
{
  g_autofree char *accounts_path =
    g_build_filename (data_home, "nuvashalauncher", "accounts.json", NULL);
  g_autoptr (NuvashaLauncherAccounts) accounts =
    nuvashalauncher_accounts_new_for_path (accounts_path);
  g_autoptr (NuvashaLauncherAccount) first = NULL;
  g_autoptr (NuvashaLauncherAccount) second = NULL;
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

  nuvashalauncher_secret_store_remove_account (first->id, NULL);
  nuvashalauncher_secret_store_remove_account (second->id, NULL);
  nuvashalauncher_secret_store_clear_active (NULL);
}

static void
test_remove_instance_command (void)
{
  g_autofree char *manager_path =
    g_build_filename (data_home, "nuvashalauncher", NULL);
  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (manager_path);
  g_autoptr (NuvashaLauncherInstanceManager) reloaded = NULL;
  g_autoptr (NuvashaLauncherInstance) instance = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *output = NULL;
  g_autofree char *directory = NULL;
  int status = -1;

  g_assert_true (nuvashalauncher_instance_manager_load (manager, &error));
  g_assert_no_error (error);
  instance = nuvashalauncher_instance_manager_create (manager, "CLI Test Instance", NULL,
                                               "1.20.1", NUVASHALAUNCHER_LOADER_VANILLA,
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

static void
test_extended_cli_commands (void)
{
  g_autofree char *manager_path =
    g_build_filename (data_home, "nuvashalauncher", NULL);
  g_autofree char *mrpack_path =
    g_build_filename (data_home, "exported.mrpack", NULL);
  g_autofree char *output = NULL;
  int status = -1;

  /* Headless stateless/error CLI checks run in parallel. */
  {
    const char *help_args[] = { "--help", NULL };
    const char *srv_args[] = { "--server", "play.example.com", NULL };
    const char *both_args[] = { "--launch", "Inst", "--server", "play.example.com",
                                "--world", "MyWorld", NULL };
    const char *cache_args[] = { "--clear-cache", NULL };
    const char *missing_launch[] = { "--launch", "DoesNotExist", "--headless", NULL };
    const char *missing_worlds[] = { "--list-worlds", "DoesNotExist", "--json", NULL };
    const char *missing_servers[] = { "--list-servers", "DoesNotExist", "--json", NULL };
    const char *missing_acct[] = { "--switch-account", "NoSuchPlayer", NULL };

    CliBatchItem batch[] = {
      { .args = help_args },
      { .args = srv_args },
      { .args = both_args },
      { .args = cache_args },
      { .args = missing_launch },
      { .args = missing_worlds },
      { .args = missing_servers },
      { .args = missing_acct },
    };
    run_cli_batch (batch, G_N_ELEMENTS (batch));

    g_assert_true (batch[0].exited && batch[0].exit_status == 0);
    g_assert_nonnull (strstr (batch[0].stdout_buf, "--create-instance"));
    g_assert_nonnull (strstr (batch[0].stdout_buf, "--server"));
    g_assert_nonnull (strstr (batch[0].stdout_buf, "--world"));
    g_assert_nonnull (strstr (batch[0].stdout_buf, "--list-worlds"));
    g_assert_nonnull (strstr (batch[0].stdout_buf, "--list-servers"));

    g_assert_true (batch[1].exited && batch[1].exit_status == 2);
    g_assert_true (batch[2].exited && batch[2].exit_status == 2);
    g_assert_true (batch[3].exited && batch[3].exit_status == 0);
    g_assert_true (batch[4].exited && batch[4].exit_status != 0);
    g_assert_true (batch[5].exited && batch[5].exit_status != 0);
    g_assert_true (batch[6].exited && batch[6].exit_status != 0);
    g_assert_true (batch[7].exited && batch[7].exit_status != 0);

    for (size_t i = 0; i < G_N_ELEMENTS (batch); i++)
      g_free (batch[i].stdout_buf);
  }

  /* --add-offline-account requires a Microsoft account first, then creates
   * an offline account and --list-accounts --json lists it. */
  {
    g_autoptr (NuvashaLauncherAccount) probe = NULL;
    g_autoptr (GError) probe_error = NULL;
    g_autofree char *accounts_path =
      g_build_filename (data_home, "nuvashalauncher", "accounts.json", NULL);
    g_autoptr (NuvashaLauncherAccounts) accounts =
      nuvashalauncher_accounts_new_for_path (accounts_path);
    g_autofree char *accounts_dir = g_path_get_dirname (accounts_path);

    g_assert_cmpint (g_mkdir_with_parents (accounts_dir, 0700), ==, 0);
    probe = g_new0 (NuvashaLauncherAccount, 1);
    probe->type = NUVASHALAUNCHER_ACCOUNT_MICROSOFT;
    probe->id = g_strdup ("069a79f444e94726a5befca90e38aaf5");
    probe->name = g_strdup ("CliMicrosoft");
    probe->uuid = g_strdup ("069a79f4-44e9-4726-a5be-fca90e38aaf5");
    probe->access_token = g_strdup ("test-token");
    probe->expires_at = G_MAXINT64;
    if (nuvashalauncher_accounts_add (accounts, probe, &probe_error))
      {
        /* Without a Microsoft account, --add-offline-account is refused. */
        g_assert_true (nuvashalauncher_accounts_remove (accounts, probe->id, NULL));
        output = run_cli ("--add-offline-account", "CliOffline", &status);
        g_assert_cmpint (status, !=, 0);
        g_clear_pointer (&output, g_free);

        /* With a Microsoft account stored, --add-offline-account succeeds. */
        g_assert_true (nuvashalauncher_accounts_add (accounts, probe, &probe_error));
        g_assert_no_error (probe_error);

        output = run_cli ("--add-offline-account", "CliOffline", &status);
        g_assert_cmpint (status, ==, 0);
        g_assert_nonnull (strstr (output, "CliOffline"));
        g_clear_pointer (&output, g_free);

        {
          const char *args[] = { "--list-accounts", "--json", NULL };
          output = run_cli_v (args, &status);
          g_assert_cmpint (status, ==, 0);
          g_assert_nonnull (strstr (output, "\"name\" : \"CliOffline\""));
          g_assert_nonnull (strstr (output, "\"type\" : \"offline\""));
          g_clear_pointer (&output, g_free);
        }

        output = run_cli ("--remove-account", "CliOffline", &status);
        g_assert_cmpint (status, ==, 0);
        g_clear_pointer (&output, g_free);

        output = run_cli ("--remove-account", "CliMicrosoft", &status);
        g_assert_cmpint (status, ==, 0);
        g_clear_pointer (&output, g_free);
      }
  }

  /* --create-instance creates a Fabric instance, --list-instances --json shows it,
   * --export-instance writes a .mrpack, and --remove-instance by name with --keep-files
   * unregisters it while preserving its directory. */
  {
    const char *args[] = { "--create-instance", "CliPack", "--mc-version", "1.20.4",
                           "--loader", "fabric", NULL };
    output = run_cli_v (args, &status);
    g_assert_cmpint (status, ==, 0);
    g_assert_nonnull (strstr (output, "CliPack"));
    g_clear_pointer (&output, g_free);
  }

  {
    const char *args[] = { "--list-instances", "--json", NULL };
    output = run_cli_v (args, &status);
    g_assert_cmpint (status, ==, 0);
    g_assert_nonnull (strstr (output, "\"name\" : \"CliPack\""));
    g_assert_nonnull (strstr (output, "\"loader\" : \"fabric\""));
    g_clear_pointer (&output, g_free);
  }

  {
    const char *args[] = { "--export-instance", "CliPack", "--output", mrpack_path, NULL };
    output = run_cli_v (args, &status);
    g_assert_cmpint (status, ==, 0);
    g_assert_true (g_file_test (mrpack_path, G_FILE_TEST_EXISTS));
    g_clear_pointer (&output, g_free);
  }

  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (manager_path);
  g_autoptr (GError) error = NULL;
  g_assert_true (nuvashalauncher_instance_manager_load (manager, &error));
  g_assert_no_error (error);
  NuvashaLauncherInstance *created =
    nuvashalauncher_instance_manager_find (manager, "CliPack", &error);
  g_assert_no_error (error);
  g_assert_nonnull (created);
  g_autofree char *created_dir = NULL;
  g_object_get (created, "directory", &created_dir, NULL);

  {
    g_autofree char *world_dir = g_build_filename (created_dir, "saves", "MyWorld", NULL);
    g_autofree char *level_dat = g_build_filename (world_dir, "level.dat", NULL);
    g_autofree char *servers_dat = g_build_filename (created_dir, "servers.dat", NULL);
    g_autoptr (NuvashaLauncherNbtTag) level_root =
      nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "");
    NuvashaLauncherNbtTag *level_data =
      nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "Data");
    g_autoptr (NuvashaLauncherNbtTag) servers_root =
      nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "");
    NuvashaLauncherNbtTag *servers_list =
      nuvashalauncher_nbt_new_list ("servers", NUVASHALAUNCHER_NBT_COMPOUND);
    NuvashaLauncherNbtTag *srv =
      nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, NULL);

    g_assert_cmpint (g_mkdir_with_parents (world_dir, 0755), ==, 0);
    nuvashalauncher_nbt_compound_set (level_data,
                                      nuvashalauncher_nbt_new_string ("LevelName", "My World"));
    nuvashalauncher_nbt_compound_set (level_root, level_data);
    g_assert_true (nuvashalauncher_nbt_write_file (level_root, level_dat, TRUE, &error));
    g_assert_no_error (error);

    nuvashalauncher_nbt_compound_set (srv,
                                      nuvashalauncher_nbt_new_string ("name", "Test Server"));
    nuvashalauncher_nbt_compound_set (srv,
                                      nuvashalauncher_nbt_new_string ("ip", "play.example.com"));
    g_ptr_array_add (servers_list->v_list.items, srv);
    nuvashalauncher_nbt_compound_set (servers_root, servers_list);
    g_assert_true (nuvashalauncher_nbt_write_file (servers_root, servers_dat, FALSE, &error));
    g_assert_no_error (error);

    const char *worlds_args[] = { "--list-worlds", "CliPack", "--json", NULL };
    const char *servers_args[] = { "--list-servers", "CliPack", "--json", NULL };
    CliBatchItem ro_batch[] = {
      { .args = worlds_args },
      { .args = servers_args },
    };
    run_cli_batch (ro_batch, G_N_ELEMENTS (ro_batch));

    g_assert_true (ro_batch[0].exited && ro_batch[0].exit_status == 0);
    g_assert_nonnull (strstr (ro_batch[0].stdout_buf, "\"folder\" : \"MyWorld\""));
    g_assert_nonnull (strstr (ro_batch[0].stdout_buf, "\"name\" : \"My World\""));
    g_free (ro_batch[0].stdout_buf);

    g_assert_true (ro_batch[1].exited && ro_batch[1].exit_status == 0);
    g_assert_nonnull (strstr (ro_batch[1].stdout_buf, "\"name\" : \"Test Server\""));
    g_assert_nonnull (strstr (ro_batch[1].stdout_buf, "\"ip\" : \"play.example.com\""));
    g_free (ro_batch[1].stdout_buf);
  }

  {
    const char *args[] = { "--remove-instance", "CliPack", "--keep-files", NULL };
    output = run_cli_v (args, &status);
    g_assert_cmpint (status, ==, 0);
    g_clear_pointer (&output, g_free);
  }
  g_assert_true (g_file_test (created_dir, G_FILE_TEST_IS_DIR));

  g_print ("ok - extended CLI commands work headlessly\n");
}

int
main (void)
{
  g_setenv ("NUVASHALAUNCHER_TEST_SECRET_STORE", "1", TRUE);
  data_home = g_dir_make_tmp ("nuvashalauncher-cli-XXXXXX", NULL);
  if (data_home == NULL)
    g_error ("could not make a temporary data directory");
  g_setenv ("XDG_DATA_HOME", data_home, TRUE);

  test_account_commands ();
  test_remove_instance_command ();
  test_extended_cli_commands ();
  nuvashalauncher_fs_remove_tree (data_home);
  g_free (data_home);
  g_print ("ok - account CLI commands list, switch and remove accounts\n");
  return EXIT_SUCCESS;
}
