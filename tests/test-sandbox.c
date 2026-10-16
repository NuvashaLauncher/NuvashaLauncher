/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "aml-fs.h"
#include "aml-sandbox.h"

static void
make_directory (const char *path)
{
  g_assert_cmpint (g_mkdir_with_parents (path, 0700), ==, 0);
}

static void
test_builds_and_runs (void)
{
  g_autofree char *root = NULL;
  g_autofree char *instance = NULL;
  g_autofree char *libraries = NULL;
  g_autofree char *assets = NULL;
  g_autofree char *natives = NULL;
  g_autofree char *version = NULL;
  g_autofree char *logging = NULL;
  g_autofree char *client = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) java_argv = NULL;
  g_autoptr (GPtrArray) sandbox_argv = NULL;
  AmlResolvedVersion resolved = { 0 };
  int seccomp_fd = -1;
  int broker_fd = -1;
  int broker_gio_fd = -1;
  char **vector;
  g_autoptr (GSubprocessLauncher) launcher = NULL;
  g_autoptr (GSubprocess) process = NULL;

  g_unsetenv ("WAYLAND_DISPLAY");
  g_unsetenv ("DISPLAY");
  root = g_dir_make_tmp ("aml-sandbox-test-XXXXXX", &error);
  g_assert_no_error (error);
  instance = g_build_filename (root, "instance", NULL);
  libraries = g_build_filename (root, "libraries", NULL);
  assets = g_build_filename (root, "assets", NULL);
  natives = g_build_filename (root, "natives", NULL);
  version = g_build_filename (root, "versions", "test", NULL);
  logging = g_build_filename (root, "logging", NULL);
  client = g_build_filename (version, "test.jar", NULL);
  make_directory (instance);
  make_directory (libraries);
  make_directory (assets);
  make_directory (g_build_filename (assets, "objects", NULL));
  make_directory (g_build_filename (assets, "indexes", NULL));
  make_directory (natives);
  make_directory (version);
  make_directory (logging);

  resolved.client_jar = client;
  resolved.library_directory = libraries;
  resolved.assets_directory = assets;
  resolved.natives_directory = natives;
  java_argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (java_argv, g_strdup ("/usr/bin/true"));
  g_ptr_array_add (java_argv, g_strdup_printf ("-Dgame=%s/world", instance));
  g_ptr_array_add (java_argv, g_strdup_printf ("-cp%s:%s", libraries, client));
  g_ptr_array_add (java_argv,
                   g_strdup_printf ("-Dskins=%s/skins/hash", assets));
  g_ptr_array_add (java_argv, g_strdup_printf ("-Djna.tmpdir=%s/jna", natives));
  g_ptr_array_add (java_argv,
                   g_strdup_printf ("-Dlwjgl=%s/lwjgl", natives));
  g_ptr_array_add (java_argv,
                   g_strdup_printf ("-Dnetty=%s/netty", natives));

  sandbox_argv = aml_sandbox_build_arguments (
      java_argv, instance, &resolved, "/usr/bin/true", TRUE, &seccomp_fd,
      -1, &broker_fd, &broker_gio_fd, &error);
  if (sandbox_argv == NULL &&
      (g_error_matches (error, AML_SANDBOX_ERROR,
                        AML_SANDBOX_ERROR_UNAVAILABLE) ||
       g_error_matches (error, AML_SANDBOX_ERROR,
                        AML_SANDBOX_ERROR_UNTRUSTED)))
    {
      g_test_skip (error->message);
      aml_fs_remove_tree (root);
      return;
    }
  g_assert_no_error (error);
  g_assert_nonnull (sandbox_argv);
  g_assert_cmpint (seccomp_fd, >=, 0);
  g_assert_true (g_path_is_absolute (g_ptr_array_index (sandbox_argv, 0)));
  g_assert_true (g_str_has_suffix (g_ptr_array_index (sandbox_argv, 0),
                                   "/bwrap"));

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_take_fd (launcher, seccomp_fd, 3);
  seccomp_fd = -1;
  vector = g_new0 (char *, sandbox_argv->len + 1);
  for (guint i = 0; i < sandbox_argv->len; i++)
    vector[i] = g_ptr_array_index (sandbox_argv, i);
  process = g_subprocess_launcher_spawnv (launcher,
                                           (const char *const *) vector,
                                           &error);
  g_free (vector);
  g_assert_no_error (error);
  g_assert_true (g_subprocess_wait_check (process, NULL, &error));
  g_assert_no_error (error);
  aml_fs_remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sandbox/builds-and-runs", test_builds_and_runs);
  return g_test_run ();
}
