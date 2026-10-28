/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-instance-manager.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>

static void
test_row_toggle_and_selection_keep_state_consistent (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root =
    g_dir_make_tmp ("nuvashalauncher-content-installed-XXXXXX", &error);
  g_autofree char *data = g_build_filename (root, "data", NULL);
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data);
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_manager_create (
      manager, "Installed", NULL, "1.21.1", nuvashalauncher_LOADER_FABRIC,
      "0.15.0", &error);
  g_autofree char *instance_directory = NULL;
  g_autofree char *mods = NULL;
  g_autofree char *enabled_path = NULL;
  g_autofree char *disabled_path = NULL;
  g_autoptr (nuvashalauncherContentPage) page = NULL;
  nuvashalauncherContentFile *file = NULL;
  GtkWidget *row = NULL;
  GtkSwitch *sw = NULL;
  GtkCheckButton *check = NULL;

  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_object_get (instance, "directory", &instance_directory, NULL);
  mods = g_build_filename (instance_directory, "mods", NULL);
  g_assert_cmpint (g_mkdir_with_parents (mods, 0700), ==, 0);
  enabled_path = g_build_filename (mods, "example.jar", NULL);
  disabled_path = g_strconcat (enabled_path,
                               nuvashalauncher_CONTENT_DISABLED_SUFFIX, NULL);
  g_assert_true (g_file_set_contents (enabled_path, "jar bytes", -1, NULL));

  page = g_object_ref_sink (nuvashalauncher_content_page_new (
    instance, nuvashalauncher_CONTENT_MOD));
  page->installed.files = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_content_file_free);
  page->installed.rows = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, NULL);
  file = g_new0 (nuvashalauncherContentFile, 1);
  file->path = g_strdup (enabled_path);
  file->filename = g_strdup ("example.jar");
  file->enabled = TRUE;
  file->project_name = g_strdup ("Example Mod");
  file->search_key = g_strdup ("example mod example.jar");
  g_ptr_array_add (page->installed.files, file);

  row = file_row (page, file);
  page->installed.rows_list = g_list_append (page->installed.rows_list, row);
  g_hash_table_insert (page->installed.rows, g_strdup (file->path), row);
  adw_preferences_group_add (page->installed.group, row);

  sw = GTK_SWITCH (g_object_get_data (G_OBJECT (row),
                                      "nuvashalauncher-mod-switch"));
  check = GTK_CHECK_BUTTON (g_object_get_data (G_OBJECT (row),
                                               "nuvashalauncher-file-check"));
  g_assert_nonnull (sw);
  g_assert_nonnull (check);

  gtk_switch_set_active (sw, FALSE);
  g_assert_false (gtk_switch_get_active (sw));
  g_assert_false (file->enabled);
  g_assert_false (g_file_test (enabled_path, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (disabled_path, G_FILE_TEST_IS_REGULAR));
  g_assert_null (g_hash_table_lookup (page->installed.rows, enabled_path));
  g_assert_true (g_hash_table_lookup (page->installed.rows, disabled_path) == row);

  gtk_check_button_set_active (check, TRUE);
  g_assert_cmpuint (g_hash_table_size (page->installed.selected_paths), ==, 1);
  g_assert_true (gtk_action_bar_get_revealed (page->installed.selection_footer));
  g_assert_true (gtk_widget_get_visible (
    GTK_WIDGET (page->installed.selection_delete)));

  gtk_check_button_set_active (check, FALSE);
  g_assert_cmpuint (g_hash_table_size (page->installed.selected_paths), ==, 0);
  g_assert_false (gtk_action_bar_get_revealed (page->installed.selection_footer));

  g_print ("ok - installed row toggles and selection preserve filesystem maps\n");
  g_clear_object (&page);
  g_remove (disabled_path);
  g_remove (mods);
  g_remove (instance_directory);
  g_remove (root);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  if (!gtk_init_check ())
    {
      g_print ("skipped - no display, cannot build installed content rows\n");
      return 77;
    }

  test_row_toggle_and_selection_keep_state_consistent ();
  return 0;
}
