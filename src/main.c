/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AML, Another Minecraft Launcher. GTK chooses the available display
 * backend at startup. */

#include <adwaita.h>
#include <gtk/gtk.h>
#include <unistd.h>
#include <string.h>

#include "aml-instance-manager.h"
#include "aml-i18n.h"
#include "aml-main-window.h"
#include "aml-download.h"
#include "aml-settings.h"

static void
on_quit (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data)
{
  g_application_quit (G_APPLICATION (user_data));
}

static const GActionEntry app_entries[] = {
  { .name = "quit", .activate = on_quit },
};

/* The half of a browser that holds the list is not a sidebar, it is the
 * same page with a project beside it: it keeps the page colour instead
 * of the sidebar colour a split view would give it.
 *
 * The first row of a list of projects is not rounded at the top, so the
 * list reads as one piece that starts where the row above it ends. */
static const char app_css[] =
  "navigation-split-view.flat-split > .sidebar-pane,"
  "navigation-split-view.flat-split > .sidebar-pane:backdrop {"
  "  background-color: transparent;"
  "  color: inherit;"
  "}"
   /* The small number on the corner of a download icon: how many files
      are picked for one install. It is round, small and pulled back over
      the corner of the icon beside it, and its colours are the text
      colour at a low alpha, so that it reads on any background and
      needs nothing from the palette. */
   "label.aml-badge {"
   "  font-size: 0.7em;"
   "  font-weight: bold;"
   "  padding: 0 0.3em;"
   "  min-width: 1.1em;"
   "  min-height: 1.1em;"
   "  border-radius: 0.8em;"
   "  background-color: alpha(currentColor, 0.25);"
   "  margin: 0;"
   "}";

static void
load_app_css (void)
{
  static gboolean loaded = FALSE;

  if (loaded)
    return;

  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_string (provider, app_css);
  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref (provider);
  loaded = TRUE;
}

static void
on_activate (GApplication *app, gpointer user_data G_GNUC_UNUSED)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  /* One window only: a second launch presents the window that exists
   * instead of opening another one. */
  if (window != NULL)
    {
      gtk_window_present (window);
      return;
    }

  load_app_css ();

  g_autoptr (AmlInstanceManager) manager = aml_instance_manager_new (NULL);
  g_autoptr (AmlSettings) settings = aml_settings_new ();
  g_autoptr (GError) error = NULL;

  aml_i18n_init (aml_settings_get_language (settings));

  if (!aml_instance_manager_load (manager, &error))
    g_warning ("cannot read the instance list: %s", error->message);

  AmlMainWindow *main_window =
    aml_main_window_new (GTK_APPLICATION (app), manager);

  gtk_window_present (GTK_WINDOW (main_window));
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = NULL;

  /* The broker can be reached through PATH (argv[0] is then usually just
   * "xdg-open") or through an absolute path mounted by Bubblewrap. */
  {
    const char *program = strrchr (argv[0], '/');
    gboolean xdg_broker =
      (program != NULL ? program + 1 : argv[0]) != NULL &&
      g_str_equal (program != NULL ? program + 1 : argv[0], "xdg-open");
    gboolean gio_broker =
      (program != NULL ? program + 1 : argv[0]) != NULL &&
      g_str_equal (program != NULL ? program + 1 : argv[0], "gio") &&
      argc == 3 && g_str_equal (argv[1], "open");

  if ((xdg_broker && argc == 2) || gio_broker)
    {
      const char *path = gio_broker ? argv[2] : argv[1];
      const char *file_path = path;
      size_t length;

      g_printerr ("AML sandbox open broker invoked: %s\n",
                  path != NULL ? path : "(null)");

      if (g_str_has_prefix (file_path, "file:"))
        {
          file_path += strlen ("file:");
          while (g_str_has_prefix (file_path, "//"))
            file_path++;
        }

      if (path == NULL || !g_str_has_prefix (file_path, "/instance/") ||
          strstr (file_path, "..") != NULL)
        {
          g_printerr ("AML sandbox open broker rejected request\n");
          return 1;
        }

      length = strlen (file_path);
      if (write (4, file_path, length) != (ssize_t) length ||
          write (4, "\n", 1) != 1)
        return 1;
      return 0;
    }
  }

  if (argc == 2 && g_strcmp0 (argv[1], "--version") == 0)
    {
      g_print ("AML %s\n", AML_VERSION);
      return 0;
    }

  if (!gtk_init_check ())
    {
      g_printerr ("AML could not open a display.\n");
      return 1;
    }

  g_set_application_name ("Another Minecraft Launcher");

  app = adw_application_new ("io.gitlab.anotherminecraftlauncher.AML",
                             G_APPLICATION_DEFAULT_FLAGS);
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_entries,
                                   G_N_ELEMENTS (app_entries), app);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  return status;
}
