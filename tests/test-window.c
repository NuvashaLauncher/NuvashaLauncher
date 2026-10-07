/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The whole window, built and taken down.
 *
 * This is where the ownership of the widgets the window hands to GTK is
 * checked: a widget that is still parented when it is disposed logs a
 * Gtk critical, and a page that is destroyed while one of its own
 * handlers is running crashes. Both have happened here before, so the
 * instance grid is opened, the window of an instance is opened, the
 * instance is renamed (which rebinds its card) and both windows are
 * closed, with every Gtk critical counted.
 *
 * Everything lives under a temporary XDG root, so the real settings and
 * accounts are never touched. No display means the test skips itself. */

#include "aml-main-window.h"
#include "aml-instance-manager.h"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <glib/gstdio.h>

static guint criticals = 0;

static GLogWriterOutput
count_criticals (GLogLevelFlags   log_level,
                 const GLogField *fields,
                 gsize            n_fields,
                 gpointer         user_data)
{
  (void) user_data;

  if ((log_level & G_LOG_LEVEL_CRITICAL) != 0)
    criticals++;

  return g_log_writer_default (log_level, fields, n_fields, NULL);
}

static GtkWidget *
find_with_data (GtkWidget *widget, const char *key)
{
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = NULL;

      if (g_object_get_data (G_OBJECT (child), key) != NULL)
        return child;

      found = find_with_data (child, key);

      if (found != NULL)
        return found;
    }

  return NULL;
}

static GtkWidget *
find_entry_titled (GtkWidget *widget, const char *title)
{
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = NULL;

      if (ADW_IS_ENTRY_ROW (child) &&
          g_strcmp0 (adw_preferences_row_get_title (
                       ADW_PREFERENCES_ROW (child)), title) == 0)
        return child;

      found = find_entry_titled (child, title);

      if (found != NULL)
        return found;
    }

  return NULL;
}

/* The window of an instance, found by the id that travels with it. */
static GtkWindow *
find_instance_window (GApplication *application, const char *id)
{
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (application));

  for (GList *item = windows; item != NULL; item = item->next)
    {
      GtkWindow *window = item->data;
      const char *other = g_object_get_data (G_OBJECT (window),
                                             "aml-instance-id");

      if (other != NULL && g_strcmp0 (other, id) == 0)
        return window;
    }

  return NULL;
}

typedef struct {
  GApplication *application;
  GtkWindow *window;          /* the window of the grid */
  GtkWindow *instance_window; /* the window of the instance */
  char *instance_id;
  guint step;
} Run;

static gboolean
finish (gpointer user_data)
{
  Run *run = user_data;

  /* Both windows are taken down while the loop still runs, so that what
     their destruction logs is counted before the test decides. */
  if (run->instance_window != NULL)
    gtk_window_destroy (run->instance_window);

  gtk_window_destroy (run->window);
  g_application_quit (run->application);

  return G_SOURCE_REMOVE;
}

static gboolean
advance (gpointer user_data)
{
  Run *run = user_data;
  GtkWidget *widget = NULL;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (
                                                  run->application));

  switch (run->step++)
    {
    case 0: /* the grid binds its card a little after the window is up */
      /* The grid is the only window so far. */
      g_assert_cmpuint (g_list_length (windows), ==, 1);
      return G_SOURCE_CONTINUE;

    case 1:
      widget = find_with_data (GTK_WIDGET (run->window), "aml-instance-id");

      if (widget == NULL)
        {
          run->step = 1;
          return G_SOURCE_CONTINUE;
        }

      g_signal_emit_by_name (widget, "clicked");
      return G_SOURCE_CONTINUE;

    case 2: /* the window of the instance is built */
      run->instance_window = find_instance_window (run->application,
                                                   run->instance_id);

      if (run->instance_window == NULL)
        {
          run->step = 2;
          return G_SOURCE_CONTINUE;
        }

      /* Opening an instance opens a window of its own, not a tab, and
         the window is named after the instance. */
      g_assert_cmpuint (g_list_length (windows), ==, 2);
      g_assert_cmpstr (gtk_window_get_title (run->instance_window), ==,
                       "Test");
      return G_SOURCE_CONTINUE;

    default:
      widget = find_entry_titled (GTK_WIDGET (run->instance_window), "Name");

      if (widget == NULL)
        {
          run->step = 3;
          return G_SOURCE_CONTINUE;
        }

      /* A rename rebinds the card: the instance leaves the store and
         comes back while its window is open. */
      gtk_editable_set_text (GTK_EDITABLE (widget), "Renamed");
      g_signal_emit_by_name (widget, "apply");

      g_timeout_add (300, finish, run);
      return G_SOURCE_REMOVE;
    }
}

static void
on_activate (GApplication *application, gpointer user_data)
{
  Run *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlInstanceManager) manager = NULL;
  g_autoptr (AmlInstance) instance = NULL;

  manager = aml_instance_manager_new (NULL);
  g_assert_nonnull (manager);

  instance = aml_instance_manager_create (manager, "Test", NULL, "1.21.1",
                                          AML_LOADER_VANILLA, NULL, &error);

  if (instance == NULL)
    g_error ("the instance cannot be created: %s", error->message);

  run->instance_id = g_strdup (aml_instance_get_id (instance));

  /* The window keeps the manager for as long as it lives. */
  run->window = GTK_WINDOW (aml_main_window_new (GTK_APPLICATION (application),
                                                 manager));
  g_assert_nonnull (run->window);

  gtk_window_present (run->window);
  g_timeout_add (200, advance, run);
}

int
main (int argc, char **argv)
{
  g_autofree char *root = NULL;
  g_autofree char *data = NULL;
  g_autofree char *config = NULL;
  g_autofree char *cache = NULL;
  g_autoptr (AdwApplication) application = NULL;
  Run run = { 0 };
  guint watchdog = 0;

  (void) argc;
  (void) argv;

  g_print ("# Window ownership\n");

#ifdef __SANITIZE_ADDRESS__
  /* GTK keeps process-global allocations that LeakSanitizer would report
     as leaks of this test. The window is checked under make test; the
     sanitizer run skips it rather than failing on the toolkit's own
     memory. */
  g_print ("skipped - the sanitizer run does not attribute Gtk's own "
           "allocations\n");
  return EXIT_SUCCESS;
#endif

  /* The settings and the accounts of the window go under this root. */
  root = g_dir_make_tmp ("aml-window-XXXXXX", NULL);

  if (root == NULL)
    {
      g_print ("skipped - no temporary directory\n");
      return EXIT_SUCCESS;
    }

  data = g_build_filename (root, "data", NULL);
  config = g_build_filename (root, "config", NULL);
  cache = g_build_filename (root, "cache", NULL);

  g_mkdir_with_parents (data, 0755);
  g_mkdir_with_parents (config, 0755);
  g_mkdir_with_parents (cache, 0755);

  g_setenv ("XDG_DATA_HOME", data, TRUE);
  g_setenv ("XDG_CONFIG_HOME", config, TRUE);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);

  if (!gtk_init_check ())
    {
      g_print ("skipped - no display, cannot build the window\n");
      return EXIT_SUCCESS;
    }

  g_log_set_writer_func (count_criticals, NULL, NULL);

  application = adw_application_new ("io.github.Flia7.AML.windowtest",
                                     G_APPLICATION_DEFAULT_FLAGS);
  run.application = G_APPLICATION (application);

  g_signal_connect (application, "activate", G_CALLBACK (on_activate), &run);

  watchdog = g_timeout_add_seconds (20, finish, &run);
  g_application_run (G_APPLICATION (application), 0, NULL);

  if (watchdog != 0)
    g_source_remove (watchdog);

  g_free (run.instance_id);

  g_assert_cmpuint (criticals, ==, 0);
  g_print ("ok - an instance opens a window and both close cleanly\n");

  return EXIT_SUCCESS;
}
