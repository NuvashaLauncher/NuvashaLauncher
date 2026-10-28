/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-download-modpack-dialog-private.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>

static void
test_details_content_renders_validated_metadata (void)
{
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-details-XXXXXX", NULL);
  g_autofree char *instances = g_build_filename (root, "instances", NULL);
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (instances);
  g_autoptr (nuvashalauncherDownloadModpackDialog) dialog =
    g_object_ref_sink (g_object_new (
      nuvashalauncher_TYPE_DOWNLOAD_MODPACK_DIALOG, NULL));
  g_autoptr (nuvashalauncherModrinthProject) project =
    g_new0 (nuvashalauncherModrinthProject, 1);
  g_autoptr (nuvashalauncherModrinthDetails) details =
    g_new0 (nuvashalauncherModrinthDetails, 1);
  g_autoptr (GtkWidget) content = NULL;

  dialog->manager = g_object_ref (manager);
  project->project_id = g_strdup ("synthetic-pack");
  project->title = g_strdup ("Synthetic Pack");
  project->description = g_strdup ("A local fixture pack.");
  project->author = g_strdup ("Fixture Author");
  project->downloads = 1234;
  dialog->project = g_steal_pointer (&project);

  details->author = g_strdup ("Fixture Author");
  details->license = g_strdup ("CC0-1.0");
  details->body = g_strdup ("A **validated** local description.");
  dialog->details = g_steal_pointer (&details);

  content = g_object_ref_sink (details_content (dialog));

  g_assert_nonnull (content);
  g_assert_true (GTK_IS_WIDGET (content));
  g_assert_nonnull (dialog->body);
  g_assert_nonnull (dialog->meta);
  g_assert_nonnull (dialog->count);
  g_assert_nonnull (dialog->dropdown);
  g_assert_nonnull (dialog->download_button);
  g_assert_cmpstr (gtk_label_get_text (dialog->meta), ==,
                   "CC0-1.0  ·  by Fixture Author");
  g_assert_true (g_str_has_suffix (gtk_label_get_text (dialog->count),
                                   "downloads"));
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (dialog->download_button)));
  g_assert_null (gtk_drop_down_get_model (dialog->dropdown));

  g_print ("ok - modpack details render synthetic validated metadata locally\n");
  g_clear_object (&dialog);
  g_remove (instances);
  g_remove (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_test_add_func ("/modpack/details-content",
                   test_details_content_renders_validated_metadata);
  return g_test_run ();
}
