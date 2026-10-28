/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-instance-manager.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>

typedef struct {
  GMainLoop *loop;
  nuvashalauncherContentPage *page;
  gboolean timed_out;
} Wait;

static gboolean
page_settled (gpointer user_data)
{
  Wait *wait = user_data;

  if (wait->page->installed.loaded &&
      !gtk_widget_get_visible (GTK_WIDGET (wait->page->installed.loading)) &&
      !wait->page->installed.identifying)
    {
      g_main_loop_quit (wait->loop);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
wait_timeout (gpointer user_data)
{
  Wait *wait = user_data;

  wait->timed_out = TRUE;
  g_main_loop_quit (wait->loop);
  return G_SOURCE_REMOVE;
}

static void
wait_for_page (nuvashalauncherContentPage *page)
{
  Wait wait = {
    .loop = g_main_loop_new (NULL, FALSE),
    .page = page,
  };
  guint settle_source = g_timeout_add (10, page_settled, &wait);
  guint timeout_source = g_timeout_add_seconds (5, wait_timeout, &wait);

  g_main_loop_run (wait.loop);

  if (wait.timed_out)
    {
      g_source_remove (settle_source);
      g_error ("content page did not settle");
    }
  else
    g_source_remove (timeout_source);

  g_main_loop_unref (wait.loop);
}

static nuvashalauncherInstanceManager *
make_manager (char **root_out, char **instance_directory_out,
              nuvashalauncherInstance **instance_out)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root =
    g_dir_make_tmp ("nuvashalauncher-content-identify-XXXXXX", &error);
  g_autofree char *data = g_build_filename (root, "data", NULL);
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data);
  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (
      manager, "Identify", NULL, "1.21.1", nuvashalauncher_LOADER_FABRIC,
      "0.15.0", &error);

  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_object_get (instance, "directory", instance_directory_out, NULL);
  *root_out = g_steal_pointer (&root);
  *instance_out = instance;
  return g_steal_pointer (&manager);
}

static void
test_empty_folder_finishes_identification (void)
{
  g_autofree char *root = NULL;
  g_autofree char *instance_directory = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = NULL;
  g_autofree char *mods = NULL;
  g_autoptr (nuvashalauncherContentPage) page = NULL;
  g_autoptr (GError) error = NULL;

  manager = make_manager (&root, &instance_directory, &instance);
  mods = g_build_filename (instance_directory, "mods", NULL);
  g_assert_cmpint (g_mkdir_with_parents (mods, 0700), ==, 0);

  page = g_object_ref_sink (nuvashalauncher_content_page_new (
    instance, nuvashalauncher_CONTENT_MOD));
  reload_installed (page);
  wait_for_page (page);

  g_assert_cmpuint (page->installed.files->len, ==, 0);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->installed.status)));
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (page->installed.scroller)));
  g_assert_false (page->installed.identifying);

  g_print ("ok - an empty content folder settles with an empty model\n");
  g_clear_object (&page);
  g_clear_object (&instance);
  g_clear_object (&manager);
  g_remove (mods);
  g_remove (instance_directory);
  g_remove (root);
}

static void
test_unreadable_folder_shows_failure_state (void)
{
  g_autofree char *root = NULL;
  g_autofree char *instance_directory = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = NULL;
  g_autofree char *mods = NULL;
  g_autoptr (nuvashalauncherContentPage) page = NULL;

  manager = make_manager (&root, &instance_directory, &instance);
  mods = g_build_filename (instance_directory, "mods", NULL);
  g_assert_true (g_file_set_contents (mods, "not a directory", -1, NULL));

  page = g_object_ref_sink (nuvashalauncher_content_page_new (
    instance, nuvashalauncher_CONTENT_MOD));
  reload_installed (page);
  wait_for_page (page);

  g_assert_null (page->installed.files);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->installed.status)));
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (page->installed.loading)));
  g_assert_false (page->installed.identifying);

  g_print ("ok - a content scan failure leaves a stable failure state\n");
  g_clear_object (&page);
  g_clear_object (&instance);
  g_clear_object (&manager);
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
      g_print ("skipped - no display, cannot build the content page\n");
      return 77;
    }

  test_empty_folder_finishes_identification ();
  test_unreadable_folder_shows_failure_state ();
  return 0;
}
