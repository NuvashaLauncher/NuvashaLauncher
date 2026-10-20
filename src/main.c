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

  g_autoptr (NuvashaInstanceManager) manager = nuvasha_instance_manager_new (NULL);
  g_autoptr (NuvashaSettings) settings = nuvasha_settings_new ();
  g_autoptr (GError) error = NULL;

  nuvasha_i18n_init (nuvasha_settings_get_language (settings));

  if (!nuvasha_instance_manager_load (manager, &error))
    g_warning ("cannot read the instance list: %s", error->message);

  NuvashaMainWindow *main_window =
    nuvasha_main_window_new (ADW_APPLICATION (app), manager);

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
