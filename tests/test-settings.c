/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-cache.h"
#include "leppoisa-settings.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static char *root;

typedef struct
{
  GMainLoop *loop;
  LeppoisaTaskScope *operation;
} CacheClearWait;

static void
cache_clear_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  CacheClearWait *wait = user_data;
  g_autoptr (GError) error = NULL;

  g_assert_true (g_task_propagate_boolean (G_TASK (result), &error));
  g_assert_no_error (error);
  leppoisa_task_scope_unref (wait->operation);
  g_main_loop_quit (wait->loop);
}

static void
test_defaults (void)
{
  g_autoptr (LeppoisaSettings) settings = leppoisa_settings_new ();

  g_assert_true (leppoisa_settings_get_cache_modrinth_api_responses (settings));
  g_assert_cmpint (leppoisa_settings_get_cache_expiration (settings), ==,
                   leppoisa_SETTINGS_CACHE_EXPIRATION_1_HOUR);
  g_assert_false (leppoisa_settings_get_sandbox_enabled (settings));
  g_assert_true (leppoisa_settings_get_sandbox_network_enabled (settings));
}

static void
test_persistence (void)
{
  g_autoptr (LeppoisaSettings) settings = leppoisa_settings_new ();
  g_autoptr (LeppoisaSettings) loaded = NULL;
  g_autofree char *config = NULL;
  g_autofree char *contents = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_true (leppoisa_settings_set_cache_expiration (
    settings, leppoisa_SETTINGS_CACHE_EXPIRATION_7_DAYS, &error));
  g_assert_no_error (error);
  g_assert_true (leppoisa_settings_set_cache_modrinth_api_responses (
    settings, FALSE, &error));
  g_assert_no_error (error);
  g_assert_true (leppoisa_settings_set_sandbox_enabled (settings, TRUE, &error));
  g_assert_no_error (error);
  g_assert_true (leppoisa_settings_set_sandbox_network_enabled (settings, FALSE,
                                                            &error));
  g_assert_no_error (error);

  loaded = leppoisa_settings_new ();
  g_assert_false (leppoisa_settings_get_cache_modrinth_api_responses (loaded));
  g_assert_cmpint (leppoisa_settings_get_cache_expiration (loaded), ==,
                   leppoisa_SETTINGS_CACHE_EXPIRATION_7_DAYS);
  g_assert_true (leppoisa_settings_get_sandbox_enabled (loaded));
  g_assert_false (leppoisa_settings_get_sandbox_network_enabled (loaded));

  config = g_build_filename (g_get_user_config_dir (), "leppoisa", "config.json",
                             NULL);
  g_assert_true (g_file_get_contents (config, &contents, NULL, &error));
  g_assert_no_error (error);
  g_assert_nonnull (strstr (contents, "cache-modrinth-api-responses"));
  g_assert_nonnull (strstr (contents, "cache-expiration"));
  g_assert_nonnull (strstr (contents, "sandbox-enabled"));
  g_assert_nonnull (strstr (contents, "sandbox-network-enabled"));
  g_assert_nonnull (strstr (contents, "\"version\""));
}

static void
test_corrupt_settings_quarantined (void)
{
  g_autofree char *config =
    g_build_filename (g_get_user_config_dir (), "leppoisa", "config.json", NULL);
  g_autofree char *backup = g_strconcat (config, ".corrupt", NULL);
  g_autoptr (LeppoisaSettings) settings = NULL;

  g_remove (config);
  g_remove (backup);
  g_assert_true (g_file_set_contents (config, "{not-json", -1, NULL));

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "*settings file*is corrupt*");
  settings = leppoisa_settings_new ();
  g_test_assert_expected_messages ();
  g_assert_true (g_file_test (backup, G_FILE_TEST_IS_REGULAR));
  g_assert_false (g_file_test (config, G_FILE_TEST_EXISTS));
  g_assert_cmpint (leppoisa_settings_get_cache_expiration (settings), ==,
                   leppoisa_SETTINGS_CACHE_EXPIRATION_1_HOUR);
}

static void
test_invalid_expiration (void)
{
  g_autoptr (LeppoisaSettings) settings = leppoisa_settings_new ();
  g_autoptr (GError) error = NULL;
  LeppoisaSettingsCacheExpiration previous =
    leppoisa_settings_get_cache_expiration (settings);

  g_assert_false (leppoisa_settings_set_cache_expiration (settings, 123, &error));
  g_assert_error (error, leppoisa_SETTINGS_ERROR, leppoisa_SETTINGS_ERROR_INVALID);
  g_assert_cmpint (leppoisa_settings_get_cache_expiration (settings), ==, previous);
}

static void
test_cache_paths (void)
{
  g_autofree char *api = leppoisa_cache_get_api_directory ();
  g_autofree char *files = leppoisa_cache_get_files_directory ();
  g_autofree char *expected_api = g_build_filename (g_get_user_cache_dir (),
                                                     "leppoisa", "api", NULL);
  g_autofree char *expected_files = g_build_filename (g_get_user_cache_dir (),
                                                       "leppoisa", "files", NULL);

  g_assert_cmpstr (api, ==, expected_api);
  g_assert_cmpstr (files, ==, expected_files);
}

static void
test_cache_cleanup (void)
{
  g_autofree char *api = leppoisa_cache_get_api_directory ();
  g_autofree char *files = leppoisa_cache_get_files_directory ();
  g_autofree char *icons = g_build_filename (g_get_user_cache_dir (), "leppoisa",
                                              "icons", NULL);
  g_autofree char *packs = g_build_filename (g_get_user_cache_dir (), "leppoisa",
                                             "packs", NULL);
  g_autofree char *instance = g_build_filename (root, "instance", NULL);
  g_autofree char *installed =
    g_build_filename (instance, "mods", "installed.jar", NULL);
  g_autofree char *instance_mods = g_path_get_dirname (installed);
  g_autofree char *api_file = g_build_filename (api, "response", NULL);
  g_autofree char *files_file = g_build_filename (files, "verified", NULL);
  g_autofree char *icons_file = g_build_filename (icons, "icon", NULL);
  g_autofree char *packs_file = g_build_filename (packs, "pack", NULL);
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);

  g_mkdir_with_parents (api, 0755);
  g_mkdir_with_parents (files, 0755);
  g_mkdir_with_parents (icons, 0755);
  g_mkdir_with_parents (packs, 0755);
  g_mkdir_with_parents (instance_mods, 0755);
  g_assert_true (g_file_set_contents (api_file, "api", -1, NULL));
  g_assert_true (g_file_set_contents (files_file, "file", -1, NULL));
  g_assert_true (g_file_set_contents (icons_file, "icon", -1, NULL));
  g_assert_true (g_file_set_contents (packs_file, "pack", -1, NULL));
  g_assert_true (g_file_set_contents (installed, "installed", -1, NULL));

   CacheClearWait wait = { loop, NULL };
   wait.operation = leppoisa_cache_clear_all_async (cache_clear_done, &wait);
  g_main_loop_run (loop);

  g_assert_false (g_file_test (api, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (files, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (icons, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (packs, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (installed, G_FILE_TEST_EXISTS));
}

int
main (int argc, char **argv)
{
  g_autofree char *config_home = NULL;
  g_autofree char *cache_home = NULL;

  g_test_init (&argc, &argv, NULL);
  root = g_dir_make_tmp ("leppoisa-settings-XXXXXX", NULL);
  g_assert_nonnull (root);
  config_home = g_build_filename (root, "config", NULL);
  cache_home = g_build_filename (root, "cache", NULL);
  g_mkdir_with_parents (config_home, 0755);
  g_mkdir_with_parents (cache_home, 0755);
  g_setenv ("XDG_CONFIG_HOME", config_home, TRUE);
  g_setenv ("XDG_CACHE_HOME", cache_home, TRUE);

  g_test_add_func ("/settings/defaults", test_defaults);
  g_test_add_func ("/settings/persistence", test_persistence);
  g_test_add_func ("/settings/corrupt-quarantine",
                   test_corrupt_settings_quarantined);
  g_test_add_func ("/settings/invalid-expiration", test_invalid_expiration);
  g_test_add_func ("/cache/paths", test_cache_paths);
  g_test_add_func ("/cache/cleanup", test_cache_cleanup);

  return g_test_run ();
}
