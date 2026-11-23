/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-instance-page.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-ui-feedback.h"

/* Instance-list row rendering and row actions. */

/* Play while the instance is idle, Stop while it is busy. */
static void
row_update (GtkWidget *play, NuvashaLauncher *launcher)
{
  gboolean busy = nuvashalauncher_launch_state_is_busy (nuvashalauncher_launcher_get_state (launcher));
  GtkWidget *icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvashalauncher-icon"));
  GtkWidget *label =
    GTK_WIDGET (g_object_get_data (G_OBJECT (play), "nuvashalauncher-label"));

  gtk_label_set_label (GTK_LABEL (label), busy ? _("Stop") : _("Play"));
  gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                busy ? "media-playback-stop-symbolic"
                                     : "media-playback-start-symbolic");
  nuvashalauncher_ui_set_action_label (GTK_WIDGET (play),
                                       busy ? _("Stop") : _("Play"));

  /* The green belongs to a game that is about to start, not to a stop:
     an idle row asks, a running one only allows stopping. */
  if (busy)
    gtk_widget_remove_css_class (GTK_WIDGET (play), "suggested-action");
  else
    gtk_widget_add_css_class (GTK_WIDGET (play), "suggested-action");
}

static void
on_row_launcher_state (NuvashaLauncher *launcher, NuvashaLauncherLaunchState state G_GNUC_UNUSED,
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
  NuvashaLauncher *launcher = NUVASHALAUNCHER_LAUNCHER (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (launcher != NULL && g_strcmp0 (response, "stop") == 0)
    nuvashalauncher_launcher_stop (launcher);

  g_clear_object (&launcher);
}

static void
confirm_stop (NuvashaLauncherMainWindow *self, NuvashaLauncher *launcher)
{
  NuvashaLauncherInstance *instance = nuvashalauncher_launcher_get_instance (launcher);
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
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  const char *id = g_object_get_data (G_OBJECT (button), "nuvashalauncher-instance-id");
  NuvashaLauncher *launcher = window_launcher (self, id);

  if (launcher == NULL)
    return;

  /* Stop busy instances; start idle ones. */
  if (nuvashalauncher_launch_state_is_busy (nuvashalauncher_launcher_get_state (launcher)))
    confirm_stop (self, launcher);
  else
    {
      NuvashaLauncherInstance *instance = nuvashalauncher_launcher_get_instance (launcher);
      g_autofree char *java_path = NULL;

      if (instance != NULL)
        g_object_get (instance, "java-path", &java_path, NULL);
      if (java_path != NULL &&
          !(g_file_test (java_path, G_FILE_TEST_IS_REGULAR) &&
            g_file_test (java_path, G_FILE_TEST_IS_EXECUTABLE)))
        nuvashalauncher_instance_manager_set_string (self->model->manager, instance,
                                          "java-path", NULL, NULL);

      nuvashalauncher_launcher_start (launcher);
    }
}

/* Puts the icon of @instance in @image, or the game square when it has
 * none or its file cannot be read. A row follows the icon of its
 * instance, so an icon chosen on the instance page and the icon a
 * modpack install gives an instance both land here. */
static void
row_set_icon (GtkImage *image, NuvashaLauncherInstance *instance)
{
  g_autofree char *path = NULL;

  g_object_get (instance, "icon", &path, NULL);

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      g_free (path);
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
      "/com/github/NuvashaLauncher/NuvashaLauncher/pack.png");
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
  row_set_icon (GTK_IMAGE (user_data), NUVASHALAUNCHER_INSTANCE (object));
}

/* One row of the instance list: an AdwActionRow of the boxed list,
 * the same kind of row the content pages and the modpack browser use,
 * so the list is sized and margined exactly like theirs. Every button
 * is wired here and carries the instance id, which is what the click
 * handlers go by. */
GtkWidget *
create_instance_row (gpointer item, gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  NuvashaLauncherInstance *instance = NUVASHALAUNCHER_INSTANCE (item);
  g_autofree char *name = NULL;
  g_autofree char *minecraft_version = NULL;
  g_autofree char *subtitle = NULL;
  NuvashaLauncherLoader loader = NUVASHALAUNCHER_LOADER_VANILLA;
  const char *id = NULL;
  const char *loader_name = NULL;
  NuvashaLauncher *launcher = NULL;
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
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), GTK_WIDGET (icon));

  gtk_image_set_pixel_size (GTK_IMAGE (play_icon), 16);
  gtk_widget_set_size_request (GTK_WIDGET (play_icon), 16, 16);
  gtk_box_append (GTK_BOX (play_box), GTK_WIDGET (play_icon));
  gtk_box_append (GTK_BOX (play_box), GTK_WIDGET (play_label));
  gtk_button_set_child (GTK_BUTTON (play), GTK_WIDGET (play_box));
  gtk_widget_set_valign (GTK_WIDGET (play), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (play), "suggested-action");
  nuvashalauncher_ui_set_action_label (GTK_WIDGET (play), _("Play"));

  /* The play state swaps the icon and the label, so they are known by
     the button itself rather than walked for. */
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-icon", play_icon);
  g_object_set_data (G_OBJECT (play), "nuvashalauncher-label", play_label);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (play));

  g_signal_connect_object (play, "clicked", G_CALLBACK (on_play_clicked), self, 0);

  /* Keep the row icon in sync with the instance while the row exists. */
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

  /* A click anywhere on the row, outside the Play button, opens the
     instance dialog. An AdwActionRow is not activatable by default. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

  return GTK_WIDGET (row);
}
