/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvasha, Nuvasha Launcher. GTK chooses the available display
 * backend at startup. */

#include <adwaita.h>
#include <gtk/gtk.h>

#include "nuvasha-instance-manager.h"
#include "nuvasha-i18n.h"
#include "nuvasha-main-window.h"
#include "nuvasha-download.h"
#include "nuvasha-settings.h"
#include "nuvasha-open-broker.h"

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
   "label.nuvasha-badge {"
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

  g_autoptr (NuvashaInstanceManager) manager = nuvasha_instance_manager_new (NULL);
  g_autoptr (NuvashaSettings) settings = nuvasha_settings_new ();
  g_autoptr (GError) error = NULL;

  nuvasha_i18n_init (nuvasha_settings_get_language (settings));

  if (!nuvasha_instance_manager_load (manager, &error))
    g_warning ("cannot read the instance list: %s", error->message);

  NuvashaMainWindow *main_window =
    nuvasha_main_window_new (GTK_APPLICATION (app), manager);

  gtk_window_present (GTK_WINDOW (main_window));
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = NULL;

  if (argc == 2 && g_strcmp0 (argv[1], "--version") == 0)
    {
      g_print ("nuvasha %s\n", nuvasha_VERSION);
      return 0;
    }

  /* This mode is mounted over xdg-open inside a game sandbox. It has no GTK
   * startup and can only forward a file URI to nuvasha's host-side broker. */
  if (argc == 2 && g_getenv ("nuvasha_OPEN_BROKER_SOCKET") != NULL)
    return nuvasha_open_broker_request (g_getenv ("nuvasha_OPEN_BROKER_SOCKET"),
                                    argv[1]);

  if (!gtk_init_check ())
    {
      g_printerr ("nuvasha could not open a display.\n");
      return 1;
    }

  g_set_application_name ("Nuvasha Launcher");

  app = adw_application_new ("com.github.nuvashalauncher.nuvasha",
                             G_APPLICATION_DEFAULT_FLAGS);
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_entries,
                                   G_N_ELEMENTS (app_entries), app);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  return status;
}
