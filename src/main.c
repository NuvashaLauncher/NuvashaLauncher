/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AML, Another Minecraft Launcher.
 *
 * Wayland only: this program never links, loads or talks to X11 or
 * XWayland.
 */

#include <adwaita.h>
#include <gtk/gtk.h>

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
   ".flush-top list.boxed-list > row:first-child {"
   "  border-top-left-radius: 0;"
   "  border-top-right-radius: 0;"
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

  /* AML runs on Wayland only. Force the Wayland backend even when the user
   * exported GDK_BACKEND=x11, and never let GDK pick another backend. */
  g_setenv ("GDK_BACKEND", "wayland", TRUE);
  gdk_set_allowed_backends ("wayland");

  /* libwayland silently falls back to a "wayland-0" socket when
   * WAYLAND_DISPLAY is unset, so require an explicit Wayland session
   * instead of relying on gtk_init_check() alone. */
  if (g_getenv ("WAYLAND_DISPLAY") == NULL || !gtk_init_check ())
    {
      g_printerr ("AML requires a Wayland session "
                  "(could not open a Wayland display).\n");
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
