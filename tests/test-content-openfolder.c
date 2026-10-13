/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Open Folder of a content page must create its folder when it does not
 * exist yet: asking the desktop to open a path that is not there gives
 * nothing back. The folder is then opened through GIO, which needs no
 * display of its own. Skips itself when no display is available for the
 * widgets. */

#include <gtk/gtk.h>
#include <libadwaita-1/adwaita.h>

#include "aml-content-page.h"
#include "aml-instance-manager.h"
#include "aml-modrinth.h"

static GtkWidget *
find_open_folder_button (GtkWidget *w)
{
  if (w == NULL)
    return NULL;

  if (GTK_IS_BUTTON (w))
    {
      /* The button is only an icon; its name is the tooltip. */
      const char *label = gtk_widget_get_tooltip_text (w);

      if (label != NULL && g_strcmp0 (label, "Open Folder") == 0)
        return w;
    }

  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
       c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_open_folder_button (c);

      if (found != NULL)
        return found;
    }

  return NULL;
}

int
main (int argc, char **argv)
{
  g_autoptr (AmlInstanceManager) manager = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = NULL;
  g_autofree char *mods_dir = NULL;
  AmlInstance *instance = NULL;
  AmlContentPage *page = NULL;

  (void) argc;
  (void) argv;

  g_print ("# Open Folder creates the content folder\n");

  if (!gtk_init_check ())
    {
      g_print ("skipped - no display, cannot build the widgets\n");
      return 77;
    }

  g_autofree char *tmp = g_dir_make_tmp ("aml-openfolder-XXXXXX", &error);
  g_assert_no_error (error);
  g_autofree char *data_dir = g_build_filename (tmp, "data", NULL);
  manager = aml_instance_manager_new (data_dir);

  instance = aml_instance_manager_create (manager, "Content", NULL, "1.21.1",
                                          AML_LOADER_FABRIC, "0.15.0", &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_object_get (instance, "directory", &directory, NULL);
  g_assert_nonnull (directory);

  mods_dir = g_build_filename (directory, "mods", NULL);
  g_assert_false (g_file_test (mods_dir, G_FILE_TEST_IS_DIR));

  page = aml_content_page_new (instance, AML_CONTENT_MOD);

  {
    GtkWidget *button = find_open_folder_button (GTK_WIDGET (page));

    g_assert_nonnull (button);
    g_signal_emit_by_name (button, "clicked");
  }

  /* The button handler creates the folder before anything is launched. */
  g_assert_true (g_file_test (mods_dir, G_FILE_TEST_IS_DIR));

  g_object_unref (instance);
  g_print ("ok - the folder is created by open folder\n");
  return EXIT_SUCCESS;
}
