/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The nuvasha main window: header bar menus and the vertical list of
 * instances, shown exactly as NuvashaInstanceManager keeps them, with an
 * empty state behind it. The Play button of a row starts the game
 * through the launcher of that instance and follows it while it runs. */

#include "nuvasha-main-window.h"
#include "nuvasha-main-window-private.h"
#include "nuvasha-cache.h"
#include "nuvasha-create-instance-dialog.h"
#include "nuvasha-download-modpack-dialog.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-instance-page.h"
#include "nuvasha-i18n.h"
#include "nuvasha-launcher.h"
#include "nuvasha-settings.h"
#include "nuvasha-users-popover.h"

G_DEFINE_FINAL_TYPE (NuvashaMainWindow, nuvasha_main_window, ADW_TYPE_APPLICATION_WINDOW)

/* Keep launch failures in the window banner until they are dismissed. */
static void
on_window_launcher_state (NuvashaLauncher *launcher G_GNUC_UNUSED,
                          NuvashaLaunchState state,
                          const char *detail, gpointer user_data)
{
  NuvashaMainWindow *self = nuvasha_MAIN_WINDOW (user_data);
  g_autofree char *message = NULL;
  const char *what = NULL;

  switch (state)
    {
    case nuvasha_LAUNCH_CRASHED:
      what = _("Minecraft crashed");
      break;

    case nuvasha_LAUNCH_FAILED:
      what = _("Minecraft could not start");
      break;

    case nuvasha_LAUNCH_NEEDS_SIGN_IN:
      what = _("Minecraft needs a sign-in");
      break;

    default:
      return;
    }

  if (self->ui.banner == NULL)
    return;

  if (detail != NULL && *detail != '\0')
    message = g_strdup_printf ("%s: %s", what, detail);
  else
    message = g_strdup (what);

  adw_banner_set_use_markup (ADW_BANNER (self->ui.banner), FALSE);
  adw_banner_set_title (ADW_BANNER (self->ui.banner), message);
  adw_banner_set_revealed (ADW_BANNER (self->ui.banner), TRUE);
}

/* The launcher of one instance, made when it is first asked for. It is
 * the one launcher the row and the page of that instance share, which
 * is what keeps a game from being started twice. */
 NuvashaLauncher *
window_launcher (NuvashaMainWindow *self, const char *instance_id)
{
  NuvashaLauncher *launcher = NULL;
  NuvashaInstance *instance = NULL;

  if (instance_id == NULL)
    return NULL;

  launcher = g_hash_table_lookup (self->model->launchers, instance_id);

  if (launcher != NULL)
    return launcher;

  instance = nuvasha_instance_manager_get_by_id (self->model->manager, instance_id);

  if (instance == NULL)
    return NULL;

  launcher = nuvasha_launcher_new (instance, self->model->settings, self->model->accounts);

  /* g_signal_connect_object drops the connection when either side goes
     away, so the window outliving a launcher is not a dangling
     pointer. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_window_launcher_state), self, 0);

  g_hash_table_insert (self->model->launchers, g_strdup (instance_id), launcher);

  return launcher;
}

static void
nuvasha_main_window_init (NuvashaMainWindow *self)
{
  self->model = nuvasha_main_window_model_new ();
  nuvasha_main_window_build_ui (self);
}


NuvashaMainWindow *
nuvasha_main_window_new (GtkApplication *application, NuvashaInstanceManager *manager)
{
  g_return_val_if_fail (GTK_IS_APPLICATION (application), NULL);
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (manager), NULL);

  NuvashaMainWindow *self = g_object_new (nuvasha_TYPE_MAIN_WINDOW,
                                      "application", application,
                                      NULL);
  GListStore *instances = nuvasha_instance_manager_get_instances (manager);
  GtkWidget *list = NULL;
  GtkWidget *scroll = NULL;

  self->model->manager = g_object_ref (manager);

  /* The standard shortcuts, registered on the application because the
     window is not the one that owns them. */
  {
    const char *create[] = { "<primary>n", NULL };
    const char *settings[] = { "<primary>comma", NULL };
    const char *shortcuts[] = { "F1", NULL };
    const char *quit[] = { "<primary>q", NULL };

    gtk_application_set_accels_for_action (application, "win.create-instance",
                                           create);
    gtk_application_set_accels_for_action (application, "win.settings",
                                           settings);
    gtk_application_set_accels_for_action (application, "win.shortcuts",
                                           shortcuts);
    gtk_application_set_accels_for_action (application, "app.quit", quit);
  }

  /* The instances are a boxed list, the same list the content pages and
     the modpack browser use, on a margin the way a boxed list is meant
     to sit: its card has round corners on every side. */
  list = gtk_list_box_new ();
  gtk_widget_add_css_class (list, "boxed-list");
  /* The card background of the list only covers its rows: the space
     under them is the window background, the same colour as the hairline
     between two rows, instead of the whole window being card coloured. */
  gtk_widget_set_valign (list, GTK_ALIGN_START);
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                   GTK_SELECTION_NONE);
  g_signal_connect (list, "row-activated", G_CALLBACK (on_row_activate),
                    self);
  gtk_list_box_bind_model (GTK_LIST_BOX (list), G_LIST_MODEL (instances),
                           create_instance_row, self, NULL);

  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
  gtk_widget_set_margin_top (scroll, 12);
  gtk_widget_set_margin_bottom (scroll, 12);
  gtk_widget_set_margin_start (scroll, 12);
  gtk_widget_set_margin_end (scroll, 12);
  gtk_widget_set_hexpand (scroll, TRUE);
  gtk_widget_set_vexpand (scroll, TRUE);
  adw_view_stack_add_named (self->ui.stack, scroll, "instances");

  /* The connection goes away with the window, which outlives every
   * callback it could receive. */
  g_signal_connect_object (instances, "items-changed",
                           G_CALLBACK (on_instances_changed), self,
                           G_CONNECT_SWAPPED);
  update_visible_page (self);

  return self;
}

static void
nuvasha_main_window_dispose (GObject *object)
{
  NuvashaMainWindow *self = nuvasha_MAIN_WINDOW (object);

  /* A running game keeps its own launcher alive through the process
     readers, so dropping the table here never kills a game in front of
     the user; it only lets go of the ones that are not running. */
  nuvasha_main_window_model_free (self->model);
  self->model = NULL;
  g_clear_pointer (&self->downloads.rows, g_hash_table_unref);

  G_OBJECT_CLASS (nuvasha_main_window_parent_class)->dispose (object);
}

static void
nuvasha_main_window_class_init (NuvashaMainWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_main_window_dispose;
}
