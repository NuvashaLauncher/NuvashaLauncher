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

static void
on_activate (GApplication *app, gpointer user_data G_GNUC_UNUSED)
{
  GtkWidget *window = adw_application_window_new (GTK_APPLICATION (app));
  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header_bar = adw_header_bar_new ();
  GtkWidget *status_page = adw_status_page_new ();

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header_bar);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), status_page);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window),
                                      toolbar_view);

  gtk_window_set_title (GTK_WINDOW (window), "AML");
  gtk_window_set_default_size (GTK_WINDOW (window), 960, 640);
  gtk_window_present (GTK_WINDOW (window));
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
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
