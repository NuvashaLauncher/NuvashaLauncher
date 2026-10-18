/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-main-window-private.h"
#include "comfy-instance-page.h"
#include "comfy-i18n.h"

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
dialog_of_instance (ComfyMainWindow *self, const char *id)
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
  ComfyMainWindow *self = comfy_MAIN_WINDOW (user_data);
  const char *id = g_object_get_data (G_OBJECT (dialog), "comfy-instance-id");

  /* A closed dialog is removed from the reopen table. Detach its page now
   * as well, so large content lists and their decoded textures are released
   * immediately instead of waiting for the dialog object to be finalized. */
  adw_dialog_set_child (dialog, NULL);

  if (id != NULL)
    {
      ComfyInstance *instance =
        comfy_instance_manager_get_by_id (self->model->manager, id);
      gulong handler = (gulong) GPOINTER_TO_SIZE (
        g_object_get_data (G_OBJECT (dialog), "comfy-rename-handler"));

      if (instance != NULL && handler != 0)
        g_signal_handler_disconnect (instance, handler);
      g_hash_table_remove (self->model->instance_dialogs, id);
    }
}

/* The dialog of an instance says the name of the instance, and says it
   again when the name changes: a dialog left saying the old name is one
   the user cannot match to anything. */
static void
on_instance_renamed (ComfyInstance *instance, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  ComfyMainWindow *self = comfy_MAIN_WINDOW (user_data);
  const char *id = comfy_instance_get_id (instance);
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
open_instance_dialog (ComfyMainWindow *self, ComfyInstance *instance)
{
  const char *id = comfy_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  GtkWidget *page = NULL;
  char *name = NULL;

  if (id == NULL)
    return;

  dialog = dialog_of_instance (self, id);

  if (dialog != NULL)
    {
      adw_dialog_present (dialog, GTK_WIDGET (self));
      return;
    }

  page = GTK_WIDGET (
    comfy_instance_page_new (self->model->manager, instance, self->model->settings,
                           window_launcher (self, id)));

  g_object_get (instance, "name", &name, NULL);

  dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, name != NULL ? name : "");
  /* The size the separate window used to have, so the lists inside have
     the room they had. */
  adw_dialog_set_content_width (dialog, 900);
  adw_dialog_set_content_height (dialog, 620);
  adw_dialog_set_child (dialog, page);
  g_free (name);

  /* The id travels with the dialog, which is what makes it findable
     again and closable when its instance is deleted. */
  g_object_set_data_full (G_OBJECT (dialog), "comfy-instance-id",
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
    g_object_set_data (G_OBJECT (dialog), "comfy-rename-handler",
                       GSIZE_TO_POINTER ((gsize) handler));
  }

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* Closes the dialog of an instance that is no longer in the list, which
 * is what deleting one does. A game that is still running keeps running:
 * its launcher is held by the table, not by the dialog. */
static void
close_dialogs_of_removed_instances (ComfyMainWindow *self)
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

      if (comfy_instance_manager_get_by_id (self->model->manager, id) == NULL)
        g_ptr_array_add (doomed, g_object_ref (dialog));
    }

  for (guint i = 0; i < doomed->len; i++)
    adw_dialog_force_close (g_ptr_array_index (doomed, i));
}

/* Play while the instance is idle, Stop while it is busy. */
static void
row_update (GtkWidget *play, ComfyLauncher *launcher)
{
  gboolean busy = comfy_launch_state_is_busy (comfy_launcher_get_state (launcher));
  GtkWidget *icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "comfy-icon"));
  GtkWidget *label =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "comfy-label"));

  gtk_label_set_label (GTK_LABEL (label), busy ? _("Stop") : _("Play"));
  gtk_image_set_from_icon_name (GTK_IMAGE (GTK_WIDGET (icon)),
                                busy ? "media-playback-stop-symbolic"
                                     : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (play), busy ? _("Stop") : _("Play"));

  /* The green belongs to a game that is about to start, not to a stop:
     an idle row asks, a running one only allows stopping. */
  if (busy)
    gtk_widget_remove_css_class (GTK_WIDGET (play), "suggested-action");
  else
    gtk_widget_add_css_class (GTK_WIDGET (play), "suggested-action");
}

static void
on_row_launcher_state (ComfyLauncher *launcher, ComfyLaunchState state G_GNUC_UNUSED,
                       const char *detail G_GNUC_UNUSED, gpointer user_data)
{
  row_update (GTK_WIDGET (user_data), launcher);
}

/* The row follows the launcher of the instance it shows. The row takes a
 * reference of its own, so a launcher that goes away first cannot leave
 * the row pointing at nothing. */
static void
row_set_launcher (GtkWidget *play, ComfyLauncher *launcher)
{
  /* Stealing keeps the reference the row took in one place instead of
   * leaving it to the notify of whatever is put there next. */
  gpointer previous = g_object_steal_data (G_OBJECT (play), "comfy-launcher");

  if (previous != NULL)
    {
      g_signal_handlers_disconnect_by_data (previous, play);
      g_object_unref (previous);
    }

  if (launcher == NULL)
    return;

  g_object_set_data_full (G_OBJECT (play), "comfy-launcher",
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
  ComfyLauncher *launcher = comfy_LAUNCHER (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (launcher != NULL && g_strcmp0 (response, "stop") == 0)
    comfy_launcher_stop (launcher);

  g_clear_object (&launcher);
}

static void
confirm_stop (ComfyMainWindow *self, ComfyLauncher *launcher)
{
  ComfyInstance *instance = comfy_launcher_get_instance (launcher);
  g_autofree char *name = NULL;
  g_autofree char *heading = NULL;
  AdwAlertDialog *alert = NULL;

  g_object_get (instance, "name", &name, NULL);
  heading = g_strdup_printf (
    "Stop %s?", name != NULL && *name != '\0' ? name : "the game");

  alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (
      heading, "The game is running and will close. Progress since the last "
               "save is lost."));
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
  ComfyMainWindow *self = comfy_MAIN_WINDOW (user_data);
  const char *id = g_object_get_data (G_OBJECT (button), "comfy-instance-id");
  ComfyLauncher *launcher = window_launcher (self, id);

  if (launcher == NULL)
    return;

  /* One button for both: what a busy instance needs is a stop, and what
     an idle one needs is a play. */
  if (comfy_launch_state_is_busy (comfy_launcher_get_state (launcher)))
    confirm_stop (self, launcher);
  else
    comfy_launcher_start (launcher);
}

/* Puts the icon of @instance in @image, or the game square when it has
 * none or its file cannot be read. A row follows the icon of its
 * instance, so an icon chosen on the instance page and the icon a
 * modpack install gives an instance both land here. */
static void
row_set_icon (GtkImage *image, ComfyInstance *instance)
{
  g_autofree char *path = NULL;

  g_object_get (instance, "icon", &path, NULL);

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      path = comfy_instance_default_icon_path ();
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

  gtk_image_set_from_icon_name (image, "applications-games-symbolic");
}

static void
on_row_icon_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  row_set_icon (GTK_IMAGE (user_data), comfy_INSTANCE (object));
}

/* One row of the instance list: an AdwActionRow of the boxed list,
 * the same kind of row the content pages and the modpack browser use,
 * so the list is sized and margined exactly like theirs. Every button
 * is wired here and carries the instance id, which is what the click
 * handlers go by. */
GtkWidget *
create_instance_row (gpointer item, gpointer user_data)
{
  ComfyMainWindow *self = comfy_MAIN_WINDOW (user_data);
  ComfyInstance *instance = comfy_INSTANCE (item);
  g_autofree char *name = NULL;
  g_autofree char *minecraft_version = NULL;
  g_autofree char *subtitle = NULL;
  ComfyLoader loader = comfy_LOADER_VANILLA;
  const char *id = NULL;
  const char *loader_name = NULL;
  ComfyLauncher *launcher = NULL;
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkImage *icon = GTK_IMAGE (gtk_image_new_from_icon_name ("applications-games-symbolic"));
  GtkButton *play = GTK_BUTTON (gtk_button_new ());
  GtkBox *play_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkImage *play_icon = GTK_IMAGE (gtk_image_new_from_icon_name ("media-playback-start-symbolic"));
  GtkLabel *play_label = GTK_LABEL (gtk_label_new (_("Play")));

  g_object_get (instance,
                "name", &name,
                "minecraft-version", &minecraft_version,
                "loader", &loader,
                NULL);

  loader_name = comfy_loader_to_string (loader);
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
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), FALSE);
  adw_action_row_set_title_lines (ADW_ACTION_ROW (GTK_WIDGET (row)), 1);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (GTK_WIDGET (row)), 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), name);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)), subtitle);

  gtk_image_set_pixel_size (GTK_IMAGE (GTK_WIDGET (icon)), 48);
  gtk_widget_set_size_request (GTK_WIDGET (icon), 48, 48);
  gtk_widget_set_valign (GTK_WIDGET (icon), GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (icon));

  gtk_image_set_pixel_size (GTK_IMAGE (GTK_WIDGET (play_icon)), 16);
  gtk_widget_set_size_request (GTK_WIDGET (play_icon), 16, 16);
  gtk_box_append (GTK_BOX (GTK_WIDGET (play_box)), GTK_WIDGET (play_icon));
  gtk_box_append (GTK_BOX (GTK_WIDGET (play_box)), GTK_WIDGET (play_label));
  gtk_button_set_child (GTK_BUTTON (GTK_WIDGET (play)), GTK_WIDGET (play_box));
  gtk_widget_set_valign (GTK_WIDGET (play), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (play), "suggested-action");
  gtk_widget_set_tooltip_text (GTK_WIDGET (play), _("Play"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (GTK_WIDGET (play)),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, _("Play"), -1);

  /* The play state swaps the icon and the label, so they are known by
     the button itself rather than walked for. */
  g_object_set_data (G_OBJECT (play), "comfy-icon", play_icon);
  g_object_set_data (G_OBJECT (play), "comfy-label", play_label);

  adw_action_row_add_suffix (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (play));

  g_signal_connect (play, "clicked", G_CALLBACK (on_play_clicked), self);

  /* The icon follows the instance's; the connection goes with the
     image, so a row that is destroyed stops following it on its own. */
  row_set_icon (GTK_IMAGE (icon), instance);
  g_signal_connect_object (instance, "notify::icon",
                           G_CALLBACK (on_row_icon_changed), icon, 0);

  id = comfy_instance_get_id (instance);

  /* The button leads back to this instance; the id is what its click
     handler goes by. */
  g_object_set_data_full (G_OBJECT (play), "comfy-instance-id", g_strdup (id),
                          g_free);

  /* The launcher of this instance: the button follows it from here on. */
  launcher = window_launcher (self, id);
  row_set_launcher (GTK_WIDGET (play), launcher);

  if (launcher != NULL)
    row_update (GTK_WIDGET (play), launcher);

  /* A click anywhere on the row, outside its buttons, opens the
     instance. An AdwActionRow is not activatable by default. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (GTK_WIDGET (row)), TRUE);

  return GTK_WIDGET (row);
}

/* A row clicked anywhere but on its buttons opens the instance. The
 * row's index comes from the boxed list, which holds the rows in the
 * same order as the manager. */
void
on_row_activate (GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row,
                 gpointer user_data)
{
  ComfyMainWindow *self = comfy_MAIN_WINDOW (user_data);
  GListStore *instances = comfy_instance_manager_get_instances (self->model->manager);
  ComfyInstance *instance =
    g_list_model_get_item (G_LIST_MODEL (instances),
                           gtk_list_box_row_get_index (row));

  if (instance == NULL)
    return;

  open_instance_dialog (self, instance);
  g_object_unref (instance);
}

void
update_visible_page (ComfyMainWindow *self)
{
  GListStore *instances = comfy_instance_manager_get_instances (self->model->manager);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (instances));

  gtk_stack_set_visible_child_name (GTK_STACK (self->ui.stack),
                                    n_items > 0 ? "instances" : "empty");
}

/* An instance that is gone leaves its launcher behind: a game running
 * for it is asked to stop, because there is nothing left to run. The
 * table holds one entry per instance, so walking it is cheap. */
static void
drop_launchers_of_removed_instances (ComfyMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;

  g_hash_table_iter_init (&iter, self->model->launchers);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      ComfyLauncher *launcher = comfy_LAUNCHER (value);

      if (comfy_instance_manager_get_by_id (self->model->manager, (const char *) key)
            != NULL)
        continue;

      comfy_launcher_stop (launcher);
      g_hash_table_iter_remove (&iter);
    }
}

void
on_instances_changed (ComfyMainWindow *self,
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
}
