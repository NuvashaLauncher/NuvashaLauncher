/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The whole window, built and taken down.
 *
 * This is where the ownership of the widgets the window hands to GTK is
 * checked: a widget that is still parented when it is disposed logs a
 * Gtk critical, and a page that is destroyed while one of its own
 * handlers is running crashes. Both have happened here before, so the
 * instance list is opened, the dialog of an instance is presented (the
 * same way the Settings dialog is), the instance is renamed (which
 * rebinds its row) and both are closed, with every Gtk critical counted.
 *
 * Everything lives under a temporary XDG root, so the real settings and
 * accounts are never touched. No display means the test skips itself. */

#include "comfy-main-window.h"
#include "comfy-instance-manager.h"

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

/* The row of the instance list: opening an instance is activating its
 * row, and the row is the only activatable one in the window. */
static GtkWidget *
find_activatable_row (GtkWidget *widget)
{
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = NULL;

      if (GTK_IS_LIST_BOX_ROW (child) &&
          gtk_list_box_row_get_activatable (GTK_LIST_BOX_ROW (child)))
        return child;

      found = find_activatable_row (child);

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

/* The dialog of an instance, found by the id that travels with it. An
 * AdwDialog is a widget, not a GtkWindow, so it lives in the widget tree
 * of the window it was presented on. */
static AdwDialog *
find_instance_dialog (GtkWidget *widget, const char *id)
{
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      AdwDialog *found = NULL;
      const char *other = NULL;

      if (ADW_IS_DIALOG (child))
        {
          other = g_object_get_data (G_OBJECT (child), "comfy-instance-id");

          if (other != NULL && g_strcmp0 (other, id) == 0)
            return ADW_DIALOG (child);
        }

      found = find_instance_dialog (child, id);

      if (found != NULL)
        return found;
    }

  return NULL;
}

typedef struct {
  GApplication *application;
  GtkWindow *window;   /* the window of the list */
  AdwDialog *dialog;   /* the dialog of the instance */
  char *instance_id;
  guint step;
  guint cycles;
  gboolean reached_dialog;
} Run;

static void on_activate (GApplication *application, gpointer user_data);

static gboolean
finish (gpointer user_data)
{
  Run *run = user_data;

  /* The dialog and the window are taken down while the loop still runs,
     so that what their destruction logs is counted before the test
     decides. */
  if (run->dialog != NULL)
    adw_dialog_force_close (run->dialog);

  gtk_window_destroy (run->window);

  if (run->cycles++ < 9)
    {
      run->window = NULL;
      run->dialog = NULL;
      g_clear_pointer (&run->instance_id, g_free);
      run->step = 0;
      on_activate (run->application, run);
      return G_SOURCE_REMOVE;
    }

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
    case 0: /* the list binds its row a little after the window is up */
      /* The window is registered with the application on its own
         schedule, which is not the first tick of the loop and is slower
         still under a sanitizer. Wait for it the same way
         the later steps wait for what they need. */
      if (g_list_length (windows) != 1)
        {
          run->step = 0;
          return G_SOURCE_CONTINUE;
        }
      return G_SOURCE_CONTINUE;

    case 1:
      widget = find_activatable_row (GTK_WIDGET (run->window));

      if (widget == NULL)
        {
          run->step = 1;
          return G_SOURCE_CONTINUE;
        }

      g_signal_emit_by_name (
        gtk_widget_get_ancestor (widget, GTK_TYPE_LIST_BOX), "row-activated",
        widget);
      return G_SOURCE_CONTINUE;

    case 2: /* the dialog of the instance is presented */
      run->dialog = find_instance_dialog (GTK_WIDGET (run->window),
                                          run->instance_id);

      if (run->dialog == NULL)
        {
          run->step = 2;
          return G_SOURCE_CONTINUE;
        }

      /* Opening an instance presents a dialog over the launcher, like
         the Settings dialog: no second window is opened, and the dialog
         is named after the instance. */
      g_assert_cmpuint (g_list_length (windows), ==, 1);
      g_assert_cmpstr (adw_dialog_get_title (run->dialog), ==, "Test");
      run->reached_dialog = TRUE;
      return G_SOURCE_CONTINUE;

    default:
      widget = find_entry_titled (GTK_WIDGET (run->dialog), "Name");

      if (widget == NULL)
        {
          run->step = 3;
          return G_SOURCE_CONTINUE;
        }

      /* A rename rebinds the row: the instance leaves the store and
         comes back while its dialog is open. */
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
  g_autoptr (ComfyInstanceManager) manager = NULL;
  g_autoptr (ComfyInstance) instance = NULL;

  manager = comfy_instance_manager_new (NULL);
  g_assert_nonnull (manager);

  instance = comfy_instance_manager_create (manager, "Test", NULL, "1.21.1",
                                          comfy_LOADER_VANILLA, NULL, &error);

  if (instance == NULL)
    g_error ("the instance cannot be created: %s", error->message);

  run->instance_id = g_strdup (comfy_instance_get_id (instance));

  /* The window keeps the manager for as long as it lives. */
  run->window = GTK_WINDOW (comfy_main_window_new (GTK_APPLICATION (application),
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
   return 77;
#endif

  /* The settings and the accounts of the window go under this root. */
  root = g_dir_make_tmp ("comfy-window-XXXXXX", NULL);

  if (root == NULL)
    {
      g_print ("skipped - no temporary directory\n");
       return 77;
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
       return 77;
    }

  g_log_set_writer_func (count_criticals, NULL, NULL);

  application =
    adw_application_new ("com.github.comfylauncher.comfy.windowtest",
                                     G_APPLICATION_DEFAULT_FLAGS);
  run.application = G_APPLICATION (application);

  g_signal_connect (application, "activate", G_CALLBACK (on_activate), &run);

  watchdog = g_timeout_add_seconds (20, finish, &run);
  g_application_run (G_APPLICATION (application), 0, NULL);

  /* The watchdog usually finishes the test long before this, but under a
     sanitizer the run can outlast it. If the watchdog fired,
     its source is already gone and removing it again is the critical
     this test is meant to catch. */
  if (watchdog != 0 && g_main_context_find_source_by_id (NULL, watchdog) != NULL)
    g_source_remove (watchdog);

  g_free (run.instance_id);

  g_assert_true (run.reached_dialog);
  g_assert_cmpuint (criticals, ==, 0);
  g_print ("ok - an instance opens a dialog and both close cleanly\n");

  return EXIT_SUCCESS;
}
