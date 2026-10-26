/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-instance-page.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-ui-feedback.h"

/* Instance-list row rendering, drag-and-drop, and row actions. */

/* Play while the instance is idle, Stop while it is busy. */
static void
row_update (GtkWidget *play, nuvashalauncher *launcher)
{
  gboolean busy = nuvashalauncher_launch_state_is_busy (nuvashalauncher_launcher_get_state (launcher));
  GtkWidget *icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvashalauncher-icon"));
  GtkWidget *label =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvashalauncher-label"));
  GtkWidget *check =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvashalauncher-check"));
  GtkWidget *instance_icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvashalauncher-instance-icon"));
  GtkWidget *edit_actions =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvashalauncher-edit-actions"));
  nuvashalauncherMainWindow *self = g_object_get_data (G_OBJECT (play), "nuvashalauncher-window");

  gtk_label_set_label (GTK_LABEL (label), busy ? _("Stop") : _("Play"));
  gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                busy ? "media-playback-stop-symbolic"
                                     : "media-playback-start-symbolic");
  nuvashalauncher_ui_set_action_label (GTK_WIDGET (play),
                                       busy ? _("Stop") : _("Play"));
  gtk_widget_set_visible (GTK_WIDGET (play), self == NULL || !self->ui.edit_mode);
  gtk_widget_set_visible (check, self != NULL && self->ui.edit_mode);
  gtk_widget_set_visible (instance_icon, self == NULL || !self->ui.edit_mode);
  gtk_widget_set_visible (edit_actions, self != NULL && self->ui.edit_mode);

  /* The green belongs to a game that is about to start, not to a stop:
     an idle row asks, a running one only allows stopping. */
  if (busy)
    gtk_widget_remove_css_class (GTK_WIDGET (play), "suggested-action");
  else
    gtk_widget_add_css_class (GTK_WIDGET (play), "suggested-action");
}

static void
on_instance_check_toggled (GtkCheckButton *check, gpointer user_data)
{
  nuvashalauncherMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (check), "nuvashalauncher-instance-id");

  if (id == NULL || !self->ui.edit_mode)
    return;

  if (gtk_check_button_get_active (check))
    g_hash_table_add (self->ui.selected_instances, g_strdup (id));
  else
    g_hash_table_remove (self->ui.selected_instances, id);

  update_instance_selection_ui (self);
}

static GdkContentProvider *
instance_row_drag_prepare (AdwActionRow *row,
                           double        x G_GNUC_UNUSED,
                           double        y G_GNUC_UNUSED)
{
  nuvashalauncherMainWindow *self = g_object_get_data (G_OBJECT (row),
                                                "nuvashalauncher-window");

  if (self == NULL || !self->ui.edit_mode)
    return NULL;

  return gdk_content_provider_new_typed (ADW_TYPE_ACTION_ROW, row);
}

static void
instance_row_drag_begin (AdwActionRow *row, GdkDrag *drag)
{
  GtkWidget *drag_list = gtk_list_box_new ();
  GtkWidget *drag_row = adw_action_row_new ();
  GtkWidget *drag_icon = gtk_image_new_from_icon_name (
    "applications-games-symbolic");
  GtkWidget *instance_icon = g_object_get_data (
    G_OBJECT (row), "nuvashalauncher-instance-icon");
  GtkWidget *drag_surface;
  int width = gtk_widget_get_width (GTK_WIDGET (row));
  int height = gtk_widget_get_height (GTK_WIDGET (row));

  gtk_widget_set_size_request (drag_list, width, height);
  gtk_widget_add_css_class (drag_list, "boxed-list");
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (drag_row), FALSE);
  adw_preferences_row_set_title (
    ADW_PREFERENCES_ROW (drag_row),
    adw_preferences_row_get_title (ADW_PREFERENCES_ROW (row)));
  adw_action_row_set_subtitle (
    ADW_ACTION_ROW (drag_row), adw_action_row_get_subtitle (row));
  if (instance_icon != NULL &&
      gtk_image_get_paintable (GTK_IMAGE (instance_icon)) != NULL)
    gtk_image_set_from_paintable (
      GTK_IMAGE (drag_icon),
      gtk_image_get_paintable (GTK_IMAGE (instance_icon)));
  gtk_widget_add_css_class (drag_icon, "nuvashalauncher-instance-icon");
  gtk_widget_set_overflow (drag_icon, GTK_OVERFLOW_HIDDEN);
  gtk_image_set_pixel_size (GTK_IMAGE (drag_icon), 48);
  adw_action_row_add_prefix (ADW_ACTION_ROW (drag_row), drag_icon);
  gtk_list_box_append (GTK_LIST_BOX (drag_list), drag_row);

  drag_surface = gtk_drag_icon_get_for_drag (drag);
  gtk_widget_add_css_class (drag_surface, "boxed-list");
  gtk_drag_icon_set_child (GTK_DRAG_ICON (drag_surface), drag_list);
}

static gboolean
instance_row_drop (AdwActionRow *target,
                   const GValue *value,
                   double        x G_GNUC_UNUSED,
                   double        y G_GNUC_UNUSED)
{
  AdwActionRow *source;
  nuvashalauncherMainWindow *self;
  GListStore *instances;
  nuvashalauncherInstance *source_instance;
  GtkWidget *source_list;
  GtkWidget *target_list;
  guint source_position;
  guint target_position;
  g_autoptr (nuvashalauncherInstance) instance = NULL;
  g_autoptr (GError) error = NULL;

  if (!G_VALUE_HOLDS (value, ADW_TYPE_ACTION_ROW))
    return FALSE;

  source = g_value_get_object (value);
  self = g_object_get_data (G_OBJECT (target), "nuvashalauncher-window");
  if (self == NULL || !self->ui.edit_mode)
    return FALSE;

  instances = nuvashalauncher_instance_manager_get_instances (self->model->manager);
  source_list = gtk_widget_get_parent (GTK_WIDGET (source));
  target_list = gtk_widget_get_parent (GTK_WIDGET (target));

  if (source_list == NULL || source_list != target_list ||
      !GTK_IS_LIST_BOX (source_list))
    return FALSE;

  source_instance = nuvashalauncher_instance_manager_get_by_id (
    self->model->manager,
    g_object_get_data (G_OBJECT (source), "nuvashalauncher-instance-id"));

  if (source == target || source_instance == NULL)
    return FALSE;

  source_position = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (source));
  target_position = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (target));
  instance = g_list_model_get_item (G_LIST_MODEL (instances), source_position);
  g_list_store_remove (instances, source_position);
  g_list_store_insert (instances, target_position, instance);

  if (!nuvashalauncher_instance_manager_save (self->model->manager, &error))
    {
      g_list_store_remove (instances, target_position);
      g_list_store_insert (instances, source_position, instance);
      adw_banner_set_title (self->ui.banner, error->message);
      adw_banner_set_revealed (self->ui.banner, TRUE);
    }

  return TRUE;
}

typedef enum {
  INSTANCE_ROW_EXPORT,
  INSTANCE_ROW_DUPLICATE,
  INSTANCE_ROW_DELETE
} InstanceRowAction;

static void
on_instance_row_action (GtkButton *button, gpointer user_data)
{
  nuvashalauncherMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (button), "nuvashalauncher-instance-id");
  InstanceRowAction action = GPOINTER_TO_INT (
    g_object_get_data (G_OBJECT (button), "nuvashalauncher-row-action"));
  nuvashalauncherInstance *instance = nuvashalauncher_instance_manager_get_by_id (
    self->model->manager, id);
  nuvashalauncherInstancePage *page;

  if (instance == NULL)
    return;

  switch (action)
    {
    case INSTANCE_ROW_EXPORT:
      /* Reuse the existing export flow, but keep its settings page out of
       * the way: the page is only an operation owner for the popup/dialog. */
      page = nuvashalauncher_instance_page_new (self->model->manager, instance,
                                        self->model->settings,
                                        window_launcher (self, id));
      g_object_ref_sink (page);
      nuvashalauncher_instance_page_trigger_export (page);
      g_object_unref (page);
      break;
    case INSTANCE_ROW_DUPLICATE:
      page = nuvashalauncher_instance_page_new (self->model->manager, instance,
                                        self->model->settings,
                                        window_launcher (self, id));
      g_object_ref_sink (page);
      nuvashalauncher_instance_page_trigger_duplicate (page);
      g_object_unref (page);
      break;
    case INSTANCE_ROW_DELETE:
      /* Use the same confirmation popup as the bulk action, without opening
       * the instance settings dialog. */
      g_hash_table_remove_all (self->ui.selected_instances);
      g_hash_table_add (self->ui.selected_instances, g_strdup (id));
      bulk_delete_instances (self);
      break;
    }
}

static void
on_row_launcher_state (nuvashalauncher *launcher, nuvashalauncherLaunchState state G_GNUC_UNUSED,
                       const char *detail G_GNUC_UNUSED, gpointer user_data)
{
  row_update (GTK_WIDGET (user_data), launcher);
}

/* The row follows the launcher of the instance it shows. The row takes a
 * reference of its own, so a launcher that goes away first cannot leave
 * the row pointing at nothing. */
static void
row_set_launcher (GtkWidget *play, nuvashalauncher *launcher)
{
  /* Stealing keeps the reference the row took in one place instead of
   * leaving it to the notify of whatever is put there next. */
  gpointer previous = g_object_steal_data (G_OBJECT (play), "nuvashalauncher-launcher");

  if (previous != NULL)
    {
      g_signal_handlers_disconnect_by_data (previous, play);
      g_object_unref (previous);
    }

  if (launcher == NULL)
    return;

  g_object_set_data_full (G_OBJECT (play), "nuvashalauncher-launcher",
                          g_object_ref (launcher), g_object_unref);

  /* The connection goes with the row, so a row that outlives several
     launchers never follows one of them twice. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_row_launcher_state), play, 0);
}

/* Stopping a running game closes it and loses whatever it has not saved,
 * so it is asked for before it happens. */
static void
on_stop_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  nuvashalauncher *launcher = nuvashalauncher_LAUNCHER (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (launcher != NULL && g_strcmp0 (response, "stop") == 0)
    nuvashalauncher_launcher_stop (launcher);

  g_clear_object (&launcher);
}

static void
confirm_stop (nuvashalauncherMainWindow *self, nuvashalauncher *launcher)
{
  nuvashalauncherInstance *instance = nuvashalauncher_launcher_get_instance (launcher);
  g_autofree char *name = NULL;
  g_autofree char *heading = NULL;
  AdwAlertDialog *alert = NULL;

  g_object_get (instance, "name", &name, NULL);
  heading = g_strdup_printf (
    _("Stop %s?"), name != NULL && *name != '\0' ? name : _("the game"));

  alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (
      heading, _("The game is running and will close. Progress since the last "
                 "save is lost.")));
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Keep Playing"));
  adw_alert_dialog_add_response (alert, "stop", _("Stop"));
  adw_alert_dialog_set_response_appearance (alert, "stop",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_stop_chosen,
                           g_object_ref (launcher));
}

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  const char *id = g_object_get_data (G_OBJECT (button), "nuvashalauncher-instance-id");
  nuvashalauncher *launcher = window_launcher (self, id);

  if (launcher == NULL)
    return;

  /* Stop busy instances; start idle ones. */
  if (nuvashalauncher_launch_state_is_busy (nuvashalauncher_launcher_get_state (launcher)))
    confirm_stop (self, launcher);
  else
    nuvashalauncher_launcher_start (launcher);
}

/* Puts the icon of @instance in @image, or the game square when it has
 * none or its file cannot be read. A row follows the icon of its
 * instance, so an icon chosen on the instance page and the icon a
 * modpack install gives an instance both land here. */
static void
row_set_icon (GtkImage *image, nuvashalauncherInstance *instance)
{
  g_autofree char *path = NULL;

  g_object_get (instance, "icon", &path, NULL);

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      path = nuvashalauncher_instance_default_icon_path ();
    }

  if (path != NULL && g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (GFile) file = g_file_new_for_path (path);
      g_autoptr (GdkTexture) texture = gdk_texture_new_from_file (file, NULL);

      if (texture != NULL)
        {
          gtk_image_set_from_paintable (image, GDK_PAINTABLE (texture));
          return;
        }
    }

  {
    g_autoptr (GdkTexture) fallback = gdk_texture_new_from_resource (
      "/com/github/nuvashalauncher/nuvashalauncher/pack.png");
    if (fallback != NULL)
      {
        gtk_image_set_from_paintable (image, GDK_PAINTABLE (fallback));
        return;
      }
  }

  gtk_image_set_from_icon_name (image, "applications-games-symbolic");
}

static void
on_row_icon_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  row_set_icon (GTK_IMAGE (user_data), nuvashalauncher_INSTANCE (object));
}

/* One row of the instance list: an AdwActionRow of the boxed list,
 * the same kind of row the content pages and the modpack browser use,
 * so the list is sized and margined exactly like theirs. Every button
 * is wired here and carries the instance id, which is what the click
 * handlers go by. */
GtkWidget *
create_instance_row (gpointer item, gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  nuvashalauncherInstance *instance = nuvashalauncher_INSTANCE (item);
  g_autofree char *name = NULL;
  g_autofree char *minecraft_version = NULL;
  g_autofree char *subtitle = NULL;
  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;
  const char *id = NULL;
  const char *loader_name = NULL;
  nuvashalauncher *launcher = NULL;
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkImage *icon = GTK_IMAGE (gtk_image_new_from_icon_name ("applications-games-symbolic"));
  GtkButton *play = GTK_BUTTON (gtk_button_new ());
  GtkCheckButton *check = GTK_CHECK_BUTTON (gtk_check_button_new ());
  GtkImage *drag_handle = GTK_IMAGE (gtk_image_new_from_icon_name (
    "list-drag-handle-symbolic"));
  GtkBox *edit_actions = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  GtkButton *row_export = GTK_BUTTON (gtk_button_new_from_icon_name (
    "document-save-symbolic"));
  GtkButton *row_duplicate = GTK_BUTTON (gtk_button_new_from_icon_name (
    "edit-copy-symbolic"));
  GtkButton *row_delete = GTK_BUTTON (gtk_button_new_from_icon_name (
    "user-trash-symbolic"));
  GtkBox *play_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkImage *play_icon = GTK_IMAGE (gtk_image_new_from_icon_name ("media-playback-start-symbolic"));
  GtkLabel *play_label = GTK_LABEL (gtk_label_new (_("Play")));

  g_object_get (instance,
                "name", &name,
                "minecraft-version", &minecraft_version,
                "loader", &loader,
                NULL);

  loader_name = nuvashalauncher_loader_to_string (loader);
  if (loader_name != NULL && loader_name[0] != '\0')
    {
      g_autofree char *capitalized = g_strdup (loader_name);

      /* "fabric" is written as "Fabric" in the row. */
      capitalized[0] = g_ascii_toupper (capitalized[0]);
      subtitle = g_strdup_printf ("%s %s", minecraft_version, capitalized);
    }
  else
    subtitle = g_strdup (minecraft_version);

  /* Names and versions are plain text and may hold an "&". */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_title_lines (ADW_ACTION_ROW (row), 1);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
   gtk_widget_add_css_class (GTK_WIDGET (row), "nuvashalauncher-instance-row");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
  gtk_widget_set_size_request (GTK_WIDGET (icon), 48, 48);
  gtk_widget_set_valign (GTK_WIDGET (icon), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (icon), "nuvashalauncher-instance-icon");
  gtk_widget_set_overflow (GTK_WIDGET (icon), GTK_OVERFLOW_HIDDEN);
  /* add_prefix prepends: add the checkbox first so the handle appears on
   * its left, matching the edit-mode order in the UI. */
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), GTK_WIDGET (check));
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), GTK_WIDGET (drag_handle));
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), GTK_WIDGET (icon));
  gtk_widget_set_visible (GTK_WIDGET (check), self->ui.edit_mode);
  gtk_widget_set_visible (GTK_WIDGET (drag_handle), self->ui.edit_mode);
  gtk_image_set_pixel_size (drag_handle, 16);
  gtk_widget_set_size_request (GTK_WIDGET (drag_handle), 16, 16);
  gtk_widget_set_valign (GTK_WIDGET (drag_handle), GTK_ALIGN_CENTER);

  gtk_image_set_pixel_size (GTK_IMAGE (play_icon), 16);
  gtk_widget_set_size_request (GTK_WIDGET (play_icon), 16, 16);
  gtk_box_append (GTK_BOX (play_box), GTK_WIDGET (play_icon));
  gtk_box_append (GTK_BOX (play_box), GTK_WIDGET (play_label));
  gtk_button_set_child (GTK_BUTTON (play), GTK_WIDGET (play_box));
  gtk_widget_set_valign (GTK_WIDGET (play), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (play), "suggested-action");
  nuvashalauncher_ui_set_action_label (GTK_WIDGET (play), _("Play"));

  /* The row controls need the ID before their signal handlers are wired. */
  id = nuvashalauncher_instance_get_id (instance);

  /* The play state swaps the icon and the label, so they are known by
     the button itself rather than walked for. */
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-icon", play_icon);
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-instance-icon", icon);
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-label", play_label);
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-check", check);
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-drag-handle", drag_handle);
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-edit-actions", edit_actions);
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-window", self);
  g_object_set_data_full (G_OBJECT (check), "nuvashalauncher-instance-id",
                          g_strdup (id), g_free);
  gtk_check_button_set_active (check, FALSE);
  g_signal_connect (check, "toggled",
                    G_CALLBACK (on_instance_check_toggled), self);

  gtk_widget_set_visible (GTK_WIDGET (edit_actions), self->ui.edit_mode);
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (row_export), _("Export"));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (row_duplicate), _("Duplicate"));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (row_delete), _("Delete"));
  gtk_box_append (edit_actions, GTK_WIDGET (row_export));
  gtk_box_append (edit_actions, GTK_WIDGET (row_duplicate));
  gtk_box_append (edit_actions, GTK_WIDGET (row_delete));
  for (guint i = 0; i < 3; i++)
    {
      GtkButton *button = GTK_BUTTON (gtk_widget_get_first_child (
        GTK_WIDGET (edit_actions)));
      for (guint j = 0; j < i; j++)
        button = GTK_BUTTON (gtk_widget_get_next_sibling (GTK_WIDGET (button)));
      g_object_set_data_full (G_OBJECT (button), "nuvashalauncher-instance-id",
                              g_strdup (id), g_free);
      g_object_set_data (G_OBJECT (button), "nuvashalauncher-row-action",
                         GINT_TO_POINTER (i));
      g_signal_connect (button, "clicked",
                        G_CALLBACK (on_instance_row_action), self);
    }

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (play));
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (edit_actions));

  g_object_set_data (G_OBJECT (row), "nuvashalauncher-window", self);
  g_object_set_data_full (G_OBJECT (row), "nuvashalauncher-instance-id",
                          g_strdup (id), g_free);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-instance-icon", icon);
  {
    GtkDragSource *source = gtk_drag_source_new ();
    GtkDropTarget *target = gtk_drop_target_new (ADW_TYPE_ACTION_ROW,
                                                  GDK_ACTION_MOVE);
    gtk_drag_source_set_actions (source, GDK_ACTION_MOVE);
    g_signal_connect_swapped (source, "prepare",
                              G_CALLBACK (instance_row_drag_prepare), row);
    g_signal_connect_swapped (source, "drag-begin",
                              G_CALLBACK (instance_row_drag_begin), row);
    /* The whole row is draggable in edit mode. Interactive child widgets
     * (checkboxes and action buttons) keep handling their own clicks. */
    gtk_widget_add_controller (GTK_WIDGET (row),
                               GTK_EVENT_CONTROLLER (source));
    gtk_drop_target_set_preload (target, TRUE);
    g_signal_connect_swapped (target, "drop",
                              G_CALLBACK (instance_row_drop), row);
    gtk_widget_add_controller (GTK_WIDGET (row),
                               GTK_EVENT_CONTROLLER (target));
  }

  g_signal_connect_object (play, "clicked", G_CALLBACK (on_play_clicked), self, 0);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-play", play);

  /* The icon follows the instance's; the connection goes with the
     image, so a row that is destroyed stops following it on its own. */
  row_set_icon (GTK_IMAGE (icon), instance);
  g_signal_connect_object (instance, "notify::icon",
                           G_CALLBACK (on_row_icon_changed), icon, 0);

  id = nuvashalauncher_instance_get_id (instance);

  /* Store the instance ID for the click handler. */
  g_object_set_data_full (G_OBJECT (play), "nuvashalauncher-instance-id", g_strdup (id),
                          g_free);

  /* The launcher of this instance: the button follows it from here on. */
  launcher = window_launcher (self, id);
  row_set_launcher (GTK_WIDGET (play), launcher);

  if (launcher != NULL)
    row_update (GTK_WIDGET (play), launcher);

  /* A click anywhere on the row, outside its buttons, opens the
     instance. An AdwActionRow is not activatable by default. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

  return GTK_WIDGET (row);
}

