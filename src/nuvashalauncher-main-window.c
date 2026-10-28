/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The nuvashalauncher main window: header bar menus and the vertical list of
 * instances, shown exactly as nuvashalauncherInstanceManager keeps them, with an
 * empty state behind it. The Play button of a row starts the game
 * through the launcher of that instance and follows it while it runs. */

#include "nuvashalauncher-main-window.h"
#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-create-instance-dialog.h"
#include "nuvashalauncher-download-modpack-dialog.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-instance-page.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-users-popover.h"

G_DEFINE_FINAL_TYPE (nuvashalauncherMainWindow, nuvashalauncher_main_window, ADW_TYPE_APPLICATION_WINDOW)

static gboolean
on_main_window_key_pressed (GtkEventControllerKey *controller G_GNUC_UNUSED,
                            guint keyval,
                            guint keycode G_GNUC_UNUSED,
                            GdkModifierType state G_GNUC_UNUSED,
                            gpointer user_data)
{
  nuvashalauncherMainWindow *self = user_data;

  if (keyval == GDK_KEY_Escape && self->ui.edit_mode)
    {
      set_instance_edit_mode (self);
      return TRUE;
    }

  return FALSE;
}

gboolean
nuvashalauncher_main_window_launch_instance (nuvashalauncherMainWindow *self,
                                     const char *instance_id,
                                     GError **error)
{
  return nuvashalauncher_main_window_launch_instance_as (self, instance_id, NULL, error);
}

gboolean
nuvashalauncher_main_window_launch_instance_as (nuvashalauncherMainWindow *self,
                                        const char *instance_id,
                                        const char *account_id,
                                        GError **error)
{
  nuvashalauncherInstance *instance;
  nuvashalauncher *launcher;

  g_return_val_if_fail (nuvashalauncher_IS_MAIN_WINDOW (self), FALSE);
  g_return_val_if_fail (instance_id != NULL && *instance_id != '\0', FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  instance = nuvashalauncher_instance_manager_get_by_id (self->model->manager,
                                                  instance_id);
  if (instance == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "no instance has ID \"%s\"", instance_id);
      return FALSE;
    }

  launcher = window_launcher (self, instance_id);
  if (launcher == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "the instance launcher could not be created");
      return FALSE;
    }

  if (account_id != NULL &&
      !nuvashalauncher_launcher_set_account (launcher, account_id, error))
    return FALSE;

  if (nuvashalauncher_launch_state_is_busy (nuvashalauncher_launcher_get_state (launcher)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "instance \"%s\" is already running", instance_id);
      return FALSE;
    }

  nuvashalauncher_launcher_start (launcher);
  return TRUE;
}

void
nuvashalauncher_main_window_import_file (nuvashalauncherMainWindow *self, const char *path)
{
  nuvashalauncherDownloadModpackDialog *dialog;

  g_return_if_fail (nuvashalauncher_IS_MAIN_WINDOW (self));
  g_return_if_fail (path != NULL && *path != '\0');

  dialog = nuvashalauncher_download_modpack_dialog_new_for_file (
    self->model->manager, path);
  g_object_ref_sink (dialog);
  g_object_unref (dialog);
}

/* Keep launch failures in the window banner until they are dismissed. */
static void
on_window_launcher_state (nuvashalauncher *launcher G_GNUC_UNUSED,
                          nuvashalauncherLaunchState state,
                          const char *detail, gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  g_autofree char *message = NULL;
  const char *what = NULL;

  switch (state)
    {
    case nuvashalauncher_LAUNCH_CRASHED:
      what = _("Minecraft crashed");
      break;

    case nuvashalauncher_LAUNCH_FAILED:
      what = _("Minecraft could not start");
      break;

    case nuvashalauncher_LAUNCH_NEEDS_SIGN_IN:
      what = _("Minecraft needs a sign-in");
      break;

    default:
      if (!nuvashalauncher_launch_state_is_busy (state))
        nuvashalauncher_main_window_maybe_quit_background (self);
      return;
    }

  if (state == nuvashalauncher_LAUNCH_CRASHED)
    {
      gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
      gtk_window_present (GTK_WINDOW (self));
      nuvashalauncher_main_window_show_crash_logs (self, launcher);
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

  if (state != nuvashalauncher_LAUNCH_CRASHED)
    nuvashalauncher_main_window_maybe_quit_background (self);
}

static gboolean
window_has_background_work (nuvashalauncherMainWindow *self)
{
  GHashTableIter iter;
  gpointer value = NULL;

  if (nuvashalauncher_downloads_count () > 0)
    return TRUE;

  if (nuvashalauncher_users_skin_background_work_active ())
    return TRUE;

  g_hash_table_iter_init (&iter, self->model->launchers);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (nuvashalauncher_launch_state_is_busy (
          nuvashalauncher_launcher_get_state (nuvashalauncher_LAUNCHER (value))))
      return TRUE;

  return FALSE;
}

void
nuvashalauncher_main_window_maybe_quit_background (nuvashalauncherMainWindow *self)
{
  if (!gtk_widget_get_visible (GTK_WIDGET (self)) &&
      !window_has_background_work (self))
    g_application_quit (G_APPLICATION (gtk_window_get_application (
      GTK_WINDOW (self))));
}

static gboolean
background_quit_tick (gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);

  if (gtk_widget_get_visible (GTK_WIDGET (self)))
    {
      self->background_quit_source = 0;
      return G_SOURCE_REMOVE;
    }

  if (!window_has_background_work (self))
    {
      self->background_quit_source = 0;
      g_application_quit (G_APPLICATION (gtk_window_get_application (
        GTK_WINDOW (self))));
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static GSettings *
window_settings_new (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema = source != NULL
    ? g_settings_schema_source_lookup (source,
                                       "com.github.nuvashalauncher.nuvashalauncher",
                                       TRUE)
    : NULL;

  if (schema == NULL)
    return NULL;

  GSettings *settings = g_settings_new_full (schema, NULL, NULL);
  g_settings_schema_unref (schema);
  return settings;
}

static gboolean
on_window_close_request (GtkWindow *window, gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  int width = 0;
  int height = 0;

  if (self->window_settings == NULL)
    return FALSE;

  if (window_has_background_work (self))
    {
      gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
      if (self->background_quit_source == 0)
        self->background_quit_source =
          g_timeout_add_seconds (1, background_quit_tick, self);
      return TRUE;
    }

  gtk_window_get_default_size (window, &width, &height);
  if (width > 0)
    g_settings_set_int (self->window_settings, "window-width", width);
  if (height > 0)
    g_settings_set_int (self->window_settings, "window-height", height);
  g_settings_set_boolean (self->window_settings, "window-maximized",
                          gtk_window_is_maximized (window));
  return FALSE;
}

/* The launcher of one instance, made when it is first asked for. It is
 * the one launcher the row and the page of that instance share, which
 * is what keeps a game from being started twice. */
 nuvashalauncher *
window_launcher (nuvashalauncherMainWindow *self, const char *instance_id)
{
  nuvashalauncher *launcher = NULL;
  nuvashalauncherInstance *instance = NULL;

  if (instance_id == NULL)
    return NULL;

  launcher = g_hash_table_lookup (self->model->launchers, instance_id);

  if (launcher != NULL)
    return launcher;

  instance = nuvashalauncher_instance_manager_get_by_id (self->model->manager, instance_id);

  if (instance == NULL)
    return NULL;

  launcher = nuvashalauncher_launcher_new (instance, self->model->settings, self->model->accounts);

  /* g_signal_connect_object drops the connection when either side goes
     away, so the window outliving a launcher is not a dangling
     pointer. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_window_launcher_state), self, 0);

  g_hash_table_insert (self->model->launchers, g_strdup (instance_id), launcher);

  return launcher;
}

static void
nuvashalauncher_main_window_init (nuvashalauncherMainWindow *self)
{
  self->model = nuvashalauncher_main_window_model_new ();
  self->ui.edit_mode = FALSE;
  self->window_settings = window_settings_new ();
  if (self->window_settings != NULL)
    {
      gtk_window_set_default_size (
        GTK_WINDOW (self),
        g_settings_get_int (self->window_settings, "window-width"),
        g_settings_get_int (self->window_settings, "window-height"));
      if (g_settings_get_boolean (self->window_settings, "window-maximized"))
        gtk_window_maximize (GTK_WINDOW (self));
    }
  g_signal_connect_object (self, "close-request",
                           G_CALLBACK (on_window_close_request), self, 0);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();
    g_signal_connect (keys, "key-pressed",
                      G_CALLBACK (on_main_window_key_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);
  }
  nuvashalauncher_main_window_build_ui (self);
}


nuvashalauncherMainWindow *
nuvashalauncher_main_window_new (AdwApplication *application, nuvashalauncherInstanceManager *manager)
{
  g_return_val_if_fail (ADW_IS_APPLICATION (application), NULL);
  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE_MANAGER (manager), NULL);

  nuvashalauncherMainWindow *self = g_object_new (nuvashalauncher_TYPE_MAIN_WINDOW,
                                      "application", application,
                                      NULL);
  GListStore *instances = nuvashalauncher_instance_manager_get_instances (manager);
   GtkListBox *list = NULL;
   GtkScrolledWindow *scroll = NULL;

  self->model->manager = g_object_ref (manager);
  self->ui.selected_instances = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        g_free, NULL);

  /* The standard shortcuts, registered on the application because the
     window is not the one that owns them. */
  {
    const char *create[] = { "<primary>n", NULL };
    const char *settings[] = { "<primary>comma", NULL };
    const char *shortcuts[] = { "F1", NULL };
    const char *quit[] = { "<primary>q", NULL };

    gtk_application_set_accels_for_action (GTK_APPLICATION (application), "win.create-instance",
                                           create);
    gtk_application_set_accels_for_action (GTK_APPLICATION (application), "win.settings",
                                           settings);
    gtk_application_set_accels_for_action (GTK_APPLICATION (application), "win.shortcuts",
                                           shortcuts);
    gtk_application_set_accels_for_action (GTK_APPLICATION (application), "app.quit", quit);
  }

  /* The instances are a boxed list, the same list the content pages and
     the modpack browser use, on a margin the way a boxed list is meant
     to sit: its card has round corners on every side. */
   list = GTK_LIST_BOX (gtk_list_box_new ());
   gtk_widget_add_css_class (GTK_WIDGET (list), "boxed-list");
   gtk_widget_add_css_class (GTK_WIDGET (list), "nuvashalauncher-list-section");
  /* The card background of the list only covers its rows: the space
     under them is the window background, the same colour as the hairline
     between two rows, instead of the whole window being card coloured. */
   gtk_widget_set_valign (GTK_WIDGET (list), GTK_ALIGN_START);
   gtk_list_box_set_selection_mode (list, GTK_SELECTION_NONE);
    g_signal_connect_object (list, "row-activated", G_CALLBACK (on_row_activate),
                             self, 0);
   gtk_list_box_bind_model (list, G_LIST_MODEL (instances),
                            create_instance_row, self, NULL);

   scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
   gtk_scrolled_window_set_child (scroll, GTK_WIDGET (list));
   gtk_widget_set_margin_top (GTK_WIDGET (scroll), 12);
   gtk_widget_set_margin_bottom (GTK_WIDGET (scroll), 12);
   gtk_widget_set_margin_start (GTK_WIDGET (scroll), 12);
   gtk_widget_set_margin_end (GTK_WIDGET (scroll), 12);
   gtk_widget_set_hexpand (GTK_WIDGET (scroll), TRUE);
   gtk_widget_set_vexpand (GTK_WIDGET (scroll), TRUE);
   adw_view_stack_add_named (self->ui.stack, GTK_WIDGET (scroll), "instances");
   self->ui.instances = list;

  /* The connection goes away with the window, which outlives every
   * callback it could receive. */
  g_signal_connect_object (instances, "items-changed",
                           G_CALLBACK (on_instances_changed), self,
                           G_CONNECT_SWAPPED);
  update_visible_page (self);

  return self;
}

static void
nuvashalauncher_main_window_dispose (GObject *object)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (object);

  if (self->background_quit_source != 0)
    {
      g_source_remove (self->background_quit_source);
      self->background_quit_source = 0;
    }

  /* A running game keeps its own launcher alive through the process
     readers, so dropping the table here never kills a game in front of
     the user; it only lets go of the ones that are not running. */
  nuvashalauncher_main_window_model_free (self->model);
  self->model = NULL;
  g_clear_object (&self->window_settings);
  g_clear_pointer (&self->ui.selected_instances, g_hash_table_unref);
  g_clear_pointer (&self->downloads.rows, g_hash_table_unref);

  G_OBJECT_CLASS (nuvashalauncher_main_window_parent_class)->dispose (object);
}

static void
nuvashalauncher_main_window_class_init (nuvashalauncherMainWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_main_window_dispose;
}
