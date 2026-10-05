/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Starts the launcher with DISPLAY and WAYLAND_DISPLAY removed and checks
 * that it refuses to run: exit code 1 and a clear message on stderr,
 * instead of falling back to X11. Runs headless. */

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static void
test_refuses_to_start_without_wayland (void)
{
  gchar *argv[] = { (gchar *) AML_BIN, NULL };
  g_auto (GStrv) env = g_get_environ ();
  g_autoptr (GError) error = NULL;
  gchar *child_stdout = NULL;
  gchar *child_stderr = NULL;
  gint status = 0;
  gboolean spawned;

  env = g_environ_unsetenv (env, "DISPLAY");
  env = g_environ_unsetenv (env, "WAYLAND_DISPLAY");

  spawned = g_spawn_sync (NULL, argv, env, G_SPAWN_DEFAULT, NULL, NULL,
                          &child_stdout, &child_stderr, &status, &error);
  g_assert_no_error (error);
  g_assert_true (spawned);

  g_assert_true (WIFEXITED (status));
  g_assert_cmpint (WEXITSTATUS (status), ==, 1);

  g_assert_nonnull (child_stderr);
  g_assert_nonnull (strstr (child_stderr, "AML requires a Wayland session"));

  g_free (child_stdout);
  g_free (child_stderr);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  test_refuses_to_start_without_wayland ();

  g_print ("ok - launcher refuses to start without a Wayland display\n");
  return EXIT_SUCCESS;
}
