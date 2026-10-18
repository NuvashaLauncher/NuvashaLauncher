/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-sandbox.h"

#include <glib.h>

static gboolean
contains (GPtrArray *arguments, const char *value)
{
  for (guint i = 0; i < arguments->len; i++)
    if (g_strcmp0 (g_ptr_array_index (arguments, i), value) == 0)
      return TRUE;

  return FALSE;
}

static void
test_wraps_command (void)
{
  g_autofree char *root = g_dir_make_tmp ("comfy-sandbox-XXXXXX", NULL);
  g_autoptr (GPtrArray) command = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) wrapped = NULL;
  g_autoptr (GPtrArray) offline_wrapped = NULL;
  ComfyResolvedVersion resolved = { 0 };
  g_autoptr (GError) error = NULL;

  if (g_find_program_in_path ("bwrap") == NULL)
    {
      g_test_skip ("bubblewrap is not installed");
      return;
    }

  g_assert_nonnull (root);
  g_ptr_array_add (command, g_strdup ("/bin/true"));
  wrapped = comfy_sandbox_wrap_arguments (command, &resolved, "/bin/true",
                                        root, TRUE, &error);

  g_assert_no_error (error);
  g_assert_nonnull (wrapped);
  g_assert_cmpstr (g_ptr_array_index (wrapped, wrapped->len - 1), ==,
                   "/bin/true");
  g_assert_true (contains (wrapped, "--unshare-pid"));
  g_assert_true (contains (wrapped, "--unshare-uts"));
  g_assert_true (contains (wrapped, "--setenv"));
  g_assert_true (contains (wrapped, "comfy_OPEN_BROKER_SOCKET"));
  g_assert_true (contains (wrapped, "/usr/bin/xdg-open"));
  g_assert_true (contains (wrapped, root));
  g_assert_false (contains (wrapped, "--unshare-net"));

  offline_wrapped = comfy_sandbox_wrap_arguments (command, &resolved, "/bin/true",
                                               root, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (offline_wrapped);
  g_assert_true (contains (offline_wrapped, "--unshare-net"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sandbox/wrap-command", test_wraps_command);
  return g_test_run ();
}
