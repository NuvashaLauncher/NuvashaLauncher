/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-main-window-private.h"
#include "nuvasha-instance-page.h"
#include "nuvasha-i18n.h"
#include "nuvasha-ui-feedback.h"

/* ---------------------------------------------------------- *
 * The dialogs of the instances
 *
 * An instance opens as an AdwDialog over the launcher, exactly like the
 * Settings dialog: it is not resizable, it cannot be moved away from the
 * window, and the page's own header bar becomes the header of the
 * dialog. That is why the instance page is not put in a window of its
 * own.
 * ---------------------------------------------------------- */

/* The dialog @id is open in, or NULL. The id travels with the dialog, so
   the table of open dialogs is all the bookkeeping there is: a dialog
   that is closed is no longer in it, and the one that holds the id is
   the one to present again. */
static AdwDialog *
dialog_of_instance (NuvashaMainWindow *self, const char *id)
{
  if (id == NULL)
    return NULL;

  return g_hash_table_lookup (self->model->instance_dialogs, id);
}

/* A dialog that was closed is not open any more. AdwDialog emits this
   while it is still alive, so the borrowed pointer leaves the table
   before the dialog is freed. */
static void
on_instance_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  NuvashaMainWindow *self = nuvasha_MAIN_WINDOW (user_data);
  const char *id = g_object_get_data (G_OBJECT (dialog), "nuvasha-instance-id");

  /* A closed dialog is removed from the reopen table. Detach its page now
   * as well, so large content lists and their decoded textures are released
   * immediately instead of waiting for the dialog object to be finalized. */
  adw_dialog_set_child (dialog, NULL);

  if (id != NULL)
    {
      NuvashaInstance *instance =
        nuvasha_instance_manager_get_by_id (self->model->manager, id);
      gulong handler = (gulong) GPOINTER_TO_SIZE (
        g_object_get_data (G_OBJECT (dialog), "nuvasha-rename-handler"));

      if (instance != NULL && handler != 0)
        g_signal_handler_disconnect (instance, handler);
      g_hash_table_remove (self->model->instance_dialogs, id);
    }
}

/* Keep the instance dialog title synchronized with the instance name. */
static void
on_instance_renamed (NuvashaInstance *instance, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  NuvashaMainWindow *self = nuvasha_MAIN_WINDOW (user_data);
  const char *id = nuvasha_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  char *name = NULL;

  if (id == NULL)
    return;

  dialog = dialog_of_instance (self, id);

  if (dialog == NULL)
    return;

  g_object_get (instance, "name", &name, NULL);
  adw_dialog_set_title (dialog, name != NULL ? name : "");
  g_free (name);
}

/* Opens @instance: the dialog it already has, brought forward, or a new
 * one over the launcher. The page's header bar carries the view switcher
 * and, because it is inside an AdwDialog, the close button of the
 * dialog. */
static void
open_instance_dialog (NuvashaMainWindow *self, NuvashaInstance *instance)
{
  const char *id = nuvasha_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  NuvashaInstancePage *page = NULL;
  char *name = NULL;

  if (id == NULL)
    return;

  dialog = dialog_of_instance (self, id);

  if (dialog != NULL)
    {
      adw_dialog_present (dialog, GTK_WIDGET (self));
      return;
    }

  page = nuvasha_instance_page_new (self->model->manager, instance,
                                    self->model->settings,
                                    window_launcher (self, id));

  g_object_get (instance, "name", &name, NULL);

  dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, name != NULL ? name : "");
  /* The size the separate window used to have, so the lists inside have
     the room they had. */
  adw_dialog_set_content_width (dialog, 900);
  adw_dialog_set_content_height (dialog, 620);
  adw_dialog_set_child (dialog, GTK_WIDGET (page));
  g_free (name);

  /* Store the instance ID so the dialog can be found after rebuilds. */
  g_object_set_data_full (G_OBJECT (dialog), "nuvasha-instance-id",
                          g_strdup (id), g_free);
  g_hash_table_insert (self->model->instance_dialogs, g_strdup (id), dialog);
  /* Weak, so that a dialog that outlives the window cannot call back
     into a window that is gone. */
  g_signal_connect_object (dialog, "closed",
                           G_CALLBACK (on_instance_dialog_closed), self, 0);

  /* The connection goes with the dialog: a page that goes away first
     leaves nothing to call back into. */
  {
     gulong handler = g_signal_connect_object (
       instance, "notify::name", G_CALLBACK (on_instance_renamed), self, 0);
    g_object_set_data (G_OBJECT (dialog), "nuvasha-rename-handler",
                       GSIZE_TO_POINTER ((gsize) handler));
  }

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

void
nuvasha_main_window_show_crash_logs (NuvashaMainWindow *self,
                                     NuvashaLauncher *launcher)
{
  NuvashaInstance *instance;
  const char *id;
  AdwDialog *dialog;
  GtkWidget *child;

  g_return_if_fail (nuvasha_IS_MAIN_WINDOW (self));
  g_return_if_fail (nuvasha_IS_LAUNCHER (launcher));

  instance = nuvasha_launcher_get_instance (launcher);
  id = instance != NULL ? nuvasha_instance_get_id (instance) : NULL;
  if (instance == NULL || id == NULL)
    return;

  open_instance_dialog (self, instance);
  dialog = dialog_of_instance (self, id);
  if (dialog == NULL)
    return;

  child = adw_dialog_get_child (dialog);
  if (child != NULL && nuvasha_IS_INSTANCE_PAGE (child))
    nuvasha_instance_page_show_logs (nuvasha_INSTANCE_PAGE (child));
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* Close dialogs for removed instances. Running launchers remain owned by the
 * window's launcher table. */
static void
close_dialogs_of_removed_instances (NuvashaMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_autoptr (GPtrArray) doomed = g_ptr_array_new_with_free_func (
    g_object_unref);

  /* Collect first: closing a dialog takes it out of the table being
     walked. */
  g_hash_table_iter_init (&iter, self->model->instance_dialogs);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *id = key;
      AdwDialog *dialog = ADW_DIALOG (value);

      if (nuvasha_instance_manager_get_by_id (self->model->manager, id) == NULL)
        g_ptr_array_add (doomed, g_object_ref (dialog));
    }

  for (guint i = 0; i < doomed->len; i++)
    adw_dialog_force_close (g_ptr_array_index (doomed, i));
}

/* Play while the instance is idle, Stop while it is busy. */
static void
row_update (GtkWidget *play, NuvashaLauncher *launcher)
{
  gboolean busy = nuvasha_launch_state_is_busy (nuvasha_launcher_get_state (launcher));
  GtkWidget *icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvasha-icon"));
  GtkWidget *label =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvasha-label"));
  GtkWidget *check =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvasha-check"));
  GtkWidget *instance_icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvasha-instance-icon"));
  GtkWidget *edit_actions =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvasha-edit-actions"));
  NuvashaMainWindow *self = g_object_get_data (G_OBJECT (play), "nuvasha-window");

  gtk_label_set_label (GTK_LABEL (label), busy ? _("Stop") : _("Play"));
  gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                busy ? "media-playback-stop-symbolic"
                                     : "media-playback-start-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (play),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  busy ? _("Stop") : _("Play"), -1);
  gtk_widget_set_tooltip_text (GTK_WIDGET (play), busy ? _("Stop") : _("Play"));
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
  NuvashaMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (check), "nuvasha-instance-id");

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
  NuvashaMainWindow *self = g_object_get_data (G_OBJECT (row),
                                                "nuvasha-window");

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
    G_OBJECT (row), "nuvasha-instance-icon");
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
  gtk_widget_add_css_class (drag_icon, "nuvasha-instance-icon");
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
  NuvashaMainWindow *self;
  GListStore *instances;
  NuvashaInstance *source_instance;
  GtkWidget *source_list;
  GtkWidget *target_list;
  guint source_position;
  guint target_position;
  g_autoptr (NuvashaInstance) instance = NULL;
  g_autoptr (GError) error = NULL;

  if (!G_VALUE_HOLDS (value, ADW_TYPE_ACTION_ROW))
    return FALSE;

  source = g_value_get_object (value);
  self = g_object_get_data (G_OBJECT (target), "nuvasha-window");
  if (self == NULL || !self->ui.edit_mode)
    return FALSE;

  instances = nuvasha_instance_manager_get_instances (self->model->manager);
  source_list = gtk_widget_get_parent (GTK_WIDGET (source));
  target_list = gtk_widget_get_parent (GTK_WIDGET (target));

  if (source_list == NULL || source_list != target_list ||
      !GTK_IS_LIST_BOX (source_list))
    return FALSE;

  source_instance = nuvasha_instance_manager_get_by_id (
    self->model->manager,
    g_object_get_data (G_OBJECT (source), "nuvasha-instance-id"));

  if (source == target || source_instance == NULL)
    return FALSE;

  source_position = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (source));
  target_position = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (target));
  instance = g_list_model_get_item (G_LIST_MODEL (instances), source_position);
  g_list_store_remove (instances, source_position);
  g_list_store_insert (instances, target_position, instance);

  if (!nuvasha_instance_manager_save (self->model->manager, &error))
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
  NuvashaMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (button), "nuvasha-instance-id");
  InstanceRowAction action = GPOINTER_TO_INT (
    g_object_get_data (G_OBJECT (button), "nuvasha-row-action"));
  NuvashaInstance *instance = nuvasha_instance_manager_get_by_id (
    self->model->manager, id);
  NuvashaInstancePage *page;

  if (instance == NULL)
    return;

  switch (action)
    {
    case INSTANCE_ROW_EXPORT:
      /* Reuse the existing export flow, but keep its settings page out of
       * the way: the page is only an operation owner for the popup/dialog. */
      page = nuvasha_instance_page_new (self->model->manager, instance,
                                        self->model->settings,
                                        window_launcher (self, id));
      g_object_ref_sink (page);
      nuvasha_instance_page_trigger_export (page);
      g_object_unref (page);
      break;
    case INSTANCE_ROW_DUPLICATE:
      page = nuvasha_instance_page_new (self->model->manager, instance,
                                        self->model->settings,
                                        window_launcher (self, id));
      g_object_ref_sink (page);
      nuvasha_instance_page_trigger_duplicate (page);
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
on_row_launcher_state (NuvashaLauncher *launcher, NuvashaLaunchState state G_GNUC_UNUSED,
                       const char *detail G_GNUC_UNUSED, gpointer user_data)
{
  row_update (GTK_WIDGET (user_data), launcher);
}

/* The row follows the launcher of the instance it shows. The row takes a
 * reference of its own, so a launcher that goes away first cannot leave
 * the row pointing at nothing. */
static void
row_set_launcher (GtkWidget *play, NuvashaLauncher *launcher)
{
  /* Stealing keeps the reference the row took in one place instead of
   * leaving it to the notify of whatever is put there next. */
  gpointer previous = g_object_steal_data (G_OBJECT (play), "nuvasha-launcher");

  if (previous != NULL)
    {
      g_signal_handlers_disconnect_by_data (previous, play);
      g_object_unref (previous);
    }

  if (launcher == NULL)
    return;

  g_object_set_data_full (G_OBJECT (play), "nuvasha-launcher",
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
  NuvashaLauncher *launcher = nuvasha_LAUNCHER (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (launcher != NULL && g_strcmp0 (response, "stop") == 0)
    nuvasha_launcher_stop (launcher);

  g_clear_object (&launcher);
}

static void
confirm_stop (NuvashaMainWindow *self, NuvashaLauncher *launcher)
{
  NuvashaInstance *instance = nuvasha_launcher_get_instance (launcher);
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
  NuvashaMainWindow *self = nuvasha_MAIN_WINDOW (user_data);
  const char *id = g_object_get_data (G_OBJECT (button), "nuvasha-instance-id");
  NuvashaLauncher *launcher = window_launcher (self, id);

  if (launcher == NULL)
    return;

  /* Stop busy instances; start idle ones. */
  if (nuvasha_launch_state_is_busy (nuvasha_launcher_get_state (launcher)))
    confirm_stop (self, launcher);
  else
    nuvasha_launcher_start (launcher);
}

/* Puts the icon of @instance in @image, or the game square when it has
 * none or its file cannot be read. A row follows the icon of its
 * instance, so an icon chosen on the instance page and the icon a
 * modpack install gives an instance both land here. */
static void
row_set_icon (GtkImage *image, NuvashaInstance *instance)
{
  g_autofree char *path = NULL;

  g_object_get (instance, "icon", &path, NULL);

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      path = nuvasha_instance_default_icon_path ();
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
      "/com/github/nuvashalauncher/nuvasha/pack.png");
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
  row_set_icon (GTK_IMAGE (user_data), nuvasha_INSTANCE (object));
}

/* One row of the instance list: an AdwActionRow of the boxed list,
 * the same kind of row the content pages and the modpack browser use,
 * so the list is sized and margined exactly like theirs. Every button
 * is wired here and carries the instance id, which is what the click
 * handlers go by. */
GtkWidget *
create_instance_row (gpointer item, gpointer user_data)
{
  NuvashaMainWindow *self = nuvasha_MAIN_WINDOW (user_data);
  NuvashaInstance *instance = nuvasha_INSTANCE (item);
  g_autofree char *name = NULL;
  g_autofree char *minecraft_version = NULL;
  g_autofree char *subtitle = NULL;
  NuvashaLoader loader = nuvasha_LOADER_VANILLA;
  const char *id = NULL;
  const char *loader_name = NULL;
  NuvashaLauncher *launcher = NULL;
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

  loader_name = nuvasha_loader_to_string (loader);
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
   gtk_widget_add_css_class (GTK_WIDGET (row), "nuvasha-instance-row");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
  gtk_widget_set_size_request (GTK_WIDGET (icon), 48, 48);
  gtk_widget_set_valign (GTK_WIDGET (icon), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (icon), "nuvasha-instance-icon");
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
  gtk_widget_set_tooltip_text (GTK_WIDGET (play), _("Play"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (play),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, _("Play"), -1);

  /* The row controls need the ID before their signal handlers are wired. */
  id = nuvasha_instance_get_id (instance);

  /* The play state swaps the icon and the label, so they are known by
     the button itself rather than walked for. */
  g_object_set_data (G_OBJECT (play), "nuvasha-icon", play_icon);
  g_object_set_data (G_OBJECT (play), "nuvasha-instance-icon", icon);
  g_object_set_data (G_OBJECT (play), "nuvasha-label", play_label);
  g_object_set_data (G_OBJECT (play), "nuvasha-check", check);
  g_object_set_data (G_OBJECT (play), "nuvasha-drag-handle", drag_handle);
  g_object_set_data (G_OBJECT (play), "nuvasha-edit-actions", edit_actions);
  g_object_set_data (G_OBJECT (play), "nuvasha-window", self);
  g_object_set_data_full (G_OBJECT (check), "nuvasha-instance-id",
                          g_strdup (id), g_free);
  gtk_check_button_set_active (check, FALSE);
  g_signal_connect (check, "toggled",
                    G_CALLBACK (on_instance_check_toggled), self);

  gtk_widget_set_visible (GTK_WIDGET (edit_actions), self->ui.edit_mode);
  nuvasha_ui_prepare_icon_button (GTK_WIDGET (row_export), _("Export"));
  nuvasha_ui_prepare_icon_button (GTK_WIDGET (row_duplicate), _("Duplicate"));
  nuvasha_ui_prepare_icon_button (GTK_WIDGET (row_delete), _("Delete"));
  gtk_box_append (edit_actions, GTK_WIDGET (row_export));
  gtk_box_append (edit_actions, GTK_WIDGET (row_duplicate));
  gtk_box_append (edit_actions, GTK_WIDGET (row_delete));
  for (guint i = 0; i < 3; i++)
    {
      GtkButton *button = GTK_BUTTON (gtk_widget_get_first_child (
        GTK_WIDGET (edit_actions)));
      for (guint j = 0; j < i; j++)
        button = GTK_BUTTON (gtk_widget_get_next_sibling (GTK_WIDGET (button)));
      g_object_set_data_full (G_OBJECT (button), "nuvasha-instance-id",
                              g_strdup (id), g_free);
      g_object_set_data (G_OBJECT (button), "nuvasha-row-action",
                         GINT_TO_POINTER (i));
      g_signal_connect (button, "clicked",
                        G_CALLBACK (on_instance_row_action), self);
    }

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (play));
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (edit_actions));

  g_object_set_data (G_OBJECT (row), "nuvasha-window", self);
  g_object_set_data_full (G_OBJECT (row), "nuvasha-instance-id",
                          g_strdup (id), g_free);
  g_object_set_data (G_OBJECT (row), "nuvasha-instance-icon", icon);
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
  g_object_set_data (G_OBJECT (row), "nuvasha-play", play);

  /* The icon follows the instance's; the connection goes with the
     image, so a row that is destroyed stops following it on its own. */
  row_set_icon (GTK_IMAGE (icon), instance);
  g_signal_connect_object (instance, "notify::icon",
                           G_CALLBACK (on_row_icon_changed), icon, 0);

  id = nuvasha_instance_get_id (instance);

  /* Store the instance ID for the click handler. */
  g_object_set_data_full (G_OBJECT (play), "nuvasha-instance-id", g_strdup (id),
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

void
update_instance_selection_ui (NuvashaMainWindow *self)
{
  guint selected = g_hash_table_size (self->ui.selected_instances);
  guint total = g_list_model_get_n_items (
    G_LIST_MODEL (nuvasha_instance_manager_get_instances (self->model->manager)));

  if (self->ui.selection_label != NULL)
    {
      g_autofree char *text = g_strdup_printf (_("%u/%u selected"), selected, total);
      gtk_label_set_text (self->ui.selection_label, text);
    }

  if (self->ui.selection_footer != NULL)
    gtk_action_bar_set_revealed (self->ui.selection_footer,
                                 self->ui.edit_mode && selected > 0);

  if (self->ui.footer_delete != NULL)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (self->ui.footer_delete), selected > 0);
      gtk_widget_set_visible (GTK_WIDGET (self->ui.footer_delete),
                              self->ui.edit_mode && selected > 0);
    }
  if (self->ui.footer_export != NULL)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (self->ui.footer_export), selected > 0);
      gtk_widget_set_visible (GTK_WIDGET (self->ui.footer_export),
                              self->ui.edit_mode && selected > 0);
    }
}

void
set_instance_edit_mode (NuvashaMainWindow *self)
{
  self->ui.edit_mode = !self->ui.edit_mode;
  if (!self->ui.edit_mode)
    g_hash_table_remove_all (self->ui.selected_instances);

  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (self->ui.instances));
       row != NULL; row = gtk_widget_get_next_sibling (row))
    {
      GtkWidget *play = g_object_get_data (G_OBJECT (row), "nuvasha-play");
      GtkWidget *check = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvasha-check") : NULL;
      GtkWidget *icon = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvasha-instance-icon") : NULL;
      GtkWidget *edit_actions = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvasha-edit-actions") : NULL;
      GtkWidget *drag_handle = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvasha-drag-handle") : NULL;

      if (play == NULL)
        continue;
      gtk_widget_set_visible (play, !self->ui.edit_mode);
      gtk_widget_set_visible (check, self->ui.edit_mode);
      gtk_widget_set_visible (icon, !self->ui.edit_mode);
      gtk_widget_set_visible (edit_actions, self->ui.edit_mode);
      gtk_widget_set_visible (drag_handle, self->ui.edit_mode);
      if (check != NULL)
        gtk_check_button_set_active (
          GTK_CHECK_BUTTON (check),
          self->ui.selected_instances != NULL &&
          g_object_get_data (G_OBJECT (check), "nuvasha-instance-id") != NULL &&
          g_hash_table_contains (self->ui.selected_instances,
                                 g_object_get_data (G_OBJECT (check),
                                                    "nuvasha-instance-id")));
    }

  update_instance_selection_ui (self);
}

static void
on_bulk_duplicate_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaMainWindow *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstance) copy =
    nuvasha_instance_manager_duplicate_finish (
      nuvasha_INSTANCE_MANAGER (source), result, &error);

  if (copy == NULL && error != NULL)
    adw_banner_set_title (self->ui.banner, error->message);
  g_object_unref (self);
}

void
bulk_duplicate_instances (NuvashaMainWindow *self)
{
  GListStore *instances = nuvasha_instance_manager_get_instances (self->model->manager);

  for (guint i = 0; i < g_list_model_get_n_items (G_LIST_MODEL (instances)); i++)
    {
      g_autoptr (NuvashaInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (instances), i);
      if (nuvasha_instance_get_id (instance) != NULL &&
          g_hash_table_contains (self->ui.selected_instances,
                                 nuvasha_instance_get_id (instance)))
        nuvasha_instance_manager_duplicate_async (self->model->manager, instance,
                                                   on_bulk_duplicate_ready,
                                                   g_object_ref (self));
    }
}

typedef struct {
  NuvashaMainWindow *window;
  NuvashaTaskScope *operation;
} BulkDeleteWait;

static void
bulk_delete_wait_free (BulkDeleteWait *wait)
{
  g_object_unref (wait->window);
  nuvasha_task_scope_unref (wait->operation);
  g_free (wait);
}

static void
on_bulk_delete_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  BulkDeleteWait *wait = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvasha_instance_manager_delete_finish (
        nuvasha_INSTANCE_MANAGER (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    adw_banner_set_title (wait->window->ui.banner, error->message);

  bulk_delete_wait_free (wait);
}

static void
on_bulk_delete_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaMainWindow *self = user_data;
  const char *response = adw_alert_dialog_choose_finish (
    ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "delete") == 0)
    {
      g_autoptr (GPtrArray) selected = g_ptr_array_new_with_free_func (g_object_unref);
      GHashTableIter iter;
      gpointer key;

      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          NuvashaInstance *instance =
            nuvasha_instance_manager_get_by_id (self->model->manager, key);
          if (instance != NULL)
            g_ptr_array_add (selected, g_object_ref (instance));
        }

      g_hash_table_remove_all (self->ui.selected_instances);
      set_instance_edit_mode (self);
      for (guint i = 0; i < selected->len; i++)
        {
          BulkDeleteWait *wait = g_new0 (BulkDeleteWait, 1);
          wait->window = g_object_ref (self);
          wait->operation = nuvasha_instance_manager_delete_async (
            self->model->manager, g_ptr_array_index (selected, i), TRUE,
            on_bulk_delete_ready, wait);
        }
    }

  g_object_unref (self);
}

static void
on_bulk_export_folder_chosen (GObject *source, GAsyncResult *result,
                              gpointer user_data)
{
  NuvashaMainWindow *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) folder = gtk_file_dialog_select_folder_finish (
    GTK_FILE_DIALOG (source), result, &error);

  if (folder != NULL)
    {
      g_autofree char *directory = g_file_get_path (folder);
      g_autoptr (GPtrArray) selected = g_ptr_array_new_with_free_func (g_free);
      GHashTableIter iter;
      gpointer key;

      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        g_ptr_array_add (selected, g_strdup (key));

      g_hash_table_remove_all (self->ui.selected_instances);
      set_instance_edit_mode (self);

      for (guint i = 0; i < selected->len; i++)
        {
          const char *id = g_ptr_array_index (selected, i);
          NuvashaInstance *instance =
            nuvasha_instance_manager_get_by_id (self->model->manager, id);
          if (instance != NULL)
            {
              g_autofree char *name = NULL;
              g_autofree char *filename = NULL;
              g_autofree char *path = NULL;
              NuvashaInstancePage *page;

              g_object_get (instance, "name", &name, NULL);
              filename = g_strdup_printf ("%s.mrpack",
                                          name != NULL ? name : "instance");
              path = g_build_filename (directory, filename, NULL);
              page = nuvasha_instance_page_new (
                self->model->manager, instance, self->model->settings,
                window_launcher (self, id));
              g_object_ref_sink (page);
              nuvasha_instance_page_export_to_path (page, path);
              g_object_unref (page);
            }
        }
    }
  else if (error != NULL &&
           !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
           !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    adw_banner_set_title (self->ui.banner, error->message);

  g_object_unref (self);
}

void
bulk_export_instances (NuvashaMainWindow *self)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Choose a Directory"));
  gtk_file_dialog_select_folder (dialog, GTK_WINDOW (self), NULL,
                                 on_bulk_export_folder_chosen,
                                 g_object_ref (self));
}

void
bulk_delete_instances (NuvashaMainWindow *self)
{
  g_autofree char *heading = NULL;
  g_autofree char *body = NULL;
  guint selected = g_hash_table_size (self->ui.selected_instances);

  if (selected > 1)
    {
      GString *names = g_string_new (NULL);
      GHashTableIter iter;
      gpointer key = NULL;

      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          NuvashaInstance *instance =
            nuvasha_instance_manager_get_by_id (self->model->manager, key);
          g_autofree char *name = NULL;

          if (instance != NULL)
            g_object_get (instance, "name", &name, NULL);
          if (name == NULL || *name == '\0')
            name = g_strdup ("Unnamed instance");
          if (names->len > 0)
            g_string_append_c (names, '\n');
          g_string_append (names, name);
        }

      body = g_strdup_printf (_("The following instances will be deleted:\n%s"),
                              names->str);
      g_string_free (names, TRUE);
    }

  if (selected == 1)
    {
      GHashTableIter iter;
      gpointer key = NULL;
      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      if (g_hash_table_iter_next (&iter, &key, NULL))
        {
          NuvashaInstance *instance =
            nuvasha_instance_manager_get_by_id (self->model->manager, key);
          g_autofree char *name = NULL;
          if (instance != NULL)
            g_object_get (instance, "name", &name, NULL);
          heading = g_strdup_printf (_("Delete \"%s\"?"),
                                     name != NULL ? name : "instance");
        }
    }

  if (heading == NULL)
    heading = g_strdup_printf (_("Delete \"%s\"?"), "selected instances");

  /* Bulk deletion is confirmed by the same destructive Adwaita alert style
   * used by the existing instance action. The actual removals remain the
   * manager's asynchronous delete operation. */
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (
    heading, body));
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "delete", _("Delete"));
  adw_alert_dialog_set_response_appearance (alert, "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_bulk_delete_chosen, g_object_ref (self));
}

/* A row clicked anywhere but on its buttons opens the instance. The
 * row's index comes from the boxed list, which holds the rows in the
 * same order as the manager. */
void
on_row_activate (GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row,
                 gpointer user_data)
{
  NuvashaMainWindow *self = nuvasha_MAIN_WINDOW (user_data);
  GListStore *instances = nuvasha_instance_manager_get_instances (self->model->manager);
  NuvashaInstance *instance =
    g_list_model_get_item (G_LIST_MODEL (instances),
                           gtk_list_box_row_get_index (row));

  if (instance == NULL)
    return;

  open_instance_dialog (self, instance);
  g_object_unref (instance);
}

void
update_visible_page (NuvashaMainWindow *self)
{
  GListStore *instances = nuvasha_instance_manager_get_instances (self->model->manager);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (instances));

  if (self->ui.edit != NULL)
    {
      if (n_items == 0 && self->ui.edit_mode)
        set_instance_edit_mode (self);
      gtk_widget_set_visible (GTK_WIDGET (self->ui.edit), n_items > 0);
    }

  adw_view_stack_set_visible_child_name (self->ui.stack,
                                         n_items > 0 ? "instances" : "empty");
}

/* An instance that is gone leaves its launcher behind: a game running
 * for it is asked to stop, because there is nothing left to run. The
 * table holds one entry per instance, so walking it is cheap. */
static void
drop_launchers_of_removed_instances (NuvashaMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;

  g_hash_table_iter_init (&iter, self->model->launchers);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      NuvashaLauncher *launcher = nuvasha_LAUNCHER (value);

      if (nuvasha_instance_manager_get_by_id (self->model->manager, (const char *) key)
            != NULL)
        continue;

      nuvasha_launcher_stop (launcher);
      g_hash_table_iter_remove (&iter);
    }
}

void
on_instances_changed (NuvashaMainWindow *self,
                      GListModel    *model G_GNUC_UNUSED,
                      guint          position G_GNUC_UNUSED,
                      guint          removed G_GNUC_UNUSED,
                      guint          added G_GNUC_UNUSED)
{
  update_visible_page (self);
  drop_launchers_of_removed_instances (self);

  /* An instance that is gone has nothing to show, so its dialog goes
     with it. This is how deleting an instance closes its dialog,
     whichever screen the delete was asked from. */
  close_dialogs_of_removed_instances (self);
  update_instance_selection_ui (self);
}
