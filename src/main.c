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
#include "aml-main-window.h"

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

  g_autoptr (AmlInstanceManager) manager = aml_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;

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

  app = adw_application_new ("io.github.Flia7.AML",
                             G_APPLICATION_DEFAULT_FLAGS);
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_entries,
                                   G_N_ELEMENTS (app_entries), app);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
