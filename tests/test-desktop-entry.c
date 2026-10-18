/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Starter test: parses the desktop entry and checks the keys comfy relies
 * on, including that no X11 specific key slipped in. Runs headless. */

#include <glib.h>
#include <stdlib.h>

static void
test_desktop_entry (void)
{
  g_autofree gchar *path =
    g_build_filename (comfy_DATA_DIR,
                      "com.github.comfylauncher.comfy.desktop", NULL);
  g_autoptr (GKeyFile) keys = g_key_file_new ();
  g_autoptr (GError) error = NULL;
  g_autofree gchar *type = NULL;
  g_autofree gchar *name = NULL;
  g_autofree gchar *comment = NULL;
  g_autofree gchar *exec = NULL;
  g_autofree gchar *icon = NULL;
  g_auto (GStrv) categories = NULL;

  g_key_file_load_from_file (keys, path, G_KEY_FILE_NONE, &error);
  g_assert_no_error (error);

  type = g_key_file_get_string (keys, "Desktop Entry", "Type", NULL);
  g_assert_cmpstr (type, ==, "Application");

  name = g_key_file_get_string (keys, "Desktop Entry", "Name", NULL);
  g_assert_cmpstr (name, ==, "Comfy Launcher");

  comment = g_key_file_get_string (keys, "Desktop Entry", "Comment", NULL);
  g_assert_cmpstr (comment, ==, "Minecraft launcher for Linux");

  exec = g_key_file_get_string (keys, "Desktop Entry", "Exec", NULL);
  g_assert_cmpstr (exec, ==, "comfy");

  icon = g_key_file_get_string (keys, "Desktop Entry", "Icon", NULL);
  g_assert_cmpstr (icon, ==, "com.github.comfylauncher.comfy");

  g_assert_true (g_key_file_has_key (keys, "Desktop Entry", "Terminal", NULL));
  g_assert_false (g_key_file_get_boolean (keys, "Desktop Entry", "Terminal",
                                          NULL));

  categories = g_key_file_get_string_list (keys, "Desktop Entry",
                                           "Categories", NULL, NULL);
  g_assert_nonnull (categories);
  g_assert_cmpstr (categories[0], ==, "Game");

  /* Keep the desktop entry independent of the display backend. */
  g_assert_false (g_key_file_has_key (keys, "Desktop Entry", "StartupWMClass",
                                      NULL));
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  test_desktop_entry ();

  g_print ("ok - desktop entry is valid and X11 free\n");
  return EXIT_SUCCESS;
}
