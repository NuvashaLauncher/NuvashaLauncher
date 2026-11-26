/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-create-instance-dialog.h"
#include "nuvashalauncher-download-modpack-dialog.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-task.h"

static void
on_create_instance (GSimpleAction *action G_GNUC_UNUSED,
                    GVariant      *parameter G_GNUC_UNUSED,
                    gpointer       user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  NuvashaLauncherCreateInstanceDialog *dialog =
    nuvashalauncher_create_instance_dialog_new (self->model->manager, self->model->settings);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_download_modpack (GSimpleAction *action G_GNUC_UNUSED,
                     GVariant      *parameter G_GNUC_UNUSED,
                     gpointer       user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  NuvashaLauncherDownloadModpackDialog *dialog =
    nuvashalauncher_download_modpack_dialog_new (self->model->manager);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_import_modpack_chosen (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED))
        nuvashalauncher_ui_error_banner (self->ui.banner, error);
      g_object_unref (self);
      return;
    }

  {
    g_autofree char *path = g_file_get_path (file);

    if (path == NULL)
      {
        nuvashalauncher_ui_error_banner (self->ui.banner, NULL);
        adw_banner_set_title (self->ui.banner,
                              _("The selected modpack file is not local."));
        adw_banner_set_revealed (self->ui.banner, TRUE);
        g_object_unref (self);
        return;
      }

    nuvashalauncher_main_window_import_file (self, path);
  }

  g_object_unref (self);
}

static void
on_import_modpack (GSimpleAction *action G_GNUC_UNUSED,
                   GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  gtk_file_dialog_set_title (dialog, _("Import modpack"));
  g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());
  gtk_file_dialog_set_initial_folder (dialog, home);
  gtk_file_filter_set_name (filter, _("Modrinth modpacks (*.mrpack)"));
  gtk_file_filter_add_suffix (filter, "mrpack");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_open (dialog,
                        GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, on_import_modpack_chosen, g_object_ref (self));
}

/* Written the moment it is switched. When the file cannot be written
 * the setting keeps the value it had, the switch goes back to it and
 * the row says why: the dialog and the file never disagree. */
static void
on_show_unstable_versions_toggled (GObject *object,
                                   GParamSpec *pspec G_GNUC_UNUSED,
                                   gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  gboolean stored =
    nuvashalauncher_settings_get_show_unstable_versions (self->model->settings);
  g_autoptr (GError) error = NULL;

  if (active == stored)
    return;

  if (nuvashalauncher_settings_set_show_unstable_versions (self->model->settings,
                                                   active, &error))
    return;

  adw_switch_row_set_active (row, stored);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), nuvashalauncher_error_message (error));
  gtk_widget_set_tooltip_text (GTK_WIDGET (row), nuvashalauncher_error_message (error));
}

/* Written the moment the value changes. A spin row can produce a value
 * for every click of its arrows, so the number that is already stored is
 * skipped rather than written over and over. */
static void
on_log_limit_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSpinRow *row = ADW_SPIN_ROW (object);
  guint limit = (guint) adw_spin_row_get_value (row);
  g_autoptr (GError) error = NULL;

  if (limit == nuvashalauncher_settings_get_log_line_limit (self->model->settings))
    return;

  /* A limit that cannot be written leaves the row showing the number the
     settings file holds. */
  if (!nuvashalauncher_settings_set_log_line_limit (self->model->settings, limit, &error))
    {
      adw_spin_row_set_value (
        row, nuvashalauncher_settings_get_log_line_limit (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

/* Same contract as the snapshots switch: written at once, and put back
   what the file says when the file says no. */
static void
on_show_unstable_loaders_toggled (GObject *object,
                                  GParamSpec *pspec G_GNUC_UNUSED,
                                  gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_show_unstable_loaders (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_show_unstable_loaders (self->model->settings, active, &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_show_unstable_loaders (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_export_metadata_toggled (GObject *object,
                            GParamSpec *pspec G_GNUC_UNUSED,
                            gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_export_metadata (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_export_metadata (self->model->settings, active, &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_export_metadata (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_delete_dependents_toggled (GObject *object,
                               GParamSpec *pspec G_GNUC_UNUSED,
                               gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_delete_dependents (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_delete_dependents (self->model->settings, active, &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_delete_dependents (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_language_selected (GObject *object,
                      GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwComboRow *row = ADW_COMBO_ROW (object);
  gsize length = 0;
  const NuvashaLauncherLanguage *languages = nuvashalauncher_i18n_languages (&length);
  guint selected = adw_combo_row_get_selected (row);
  g_autoptr (GError) error = NULL;

  if (selected >= length)
    return;

  if (!nuvashalauncher_settings_set_language (self->model->settings, languages[selected].code, &error))
    nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
}

static void
on_cache_modrinth_api_responses_toggled (GObject *object,
                                         GParamSpec *pspec G_GNUC_UNUSED,
                                         gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_cache_modrinth_api_responses (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_cache_modrinth_api_responses (self->model->settings, active,
                                                      &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_cache_modrinth_api_responses (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_cache_expiration_selected (GObject *object,
                               GParamSpec *pspec G_GNUC_UNUSED,
                               gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwComboRow *row = ADW_COMBO_ROW (object);
  static const NuvashaLauncherSettingsCacheExpiration values[] = {
    NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_1_HOUR,
    NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_6_HOURS,
    NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_1_DAY,
    NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_7_DAYS
  };
  guint selected = adw_combo_row_get_selected (row);
  g_autoptr (GError) error = NULL;

  if (selected >= G_N_ELEMENTS (values) ||
      values[selected] == nuvashalauncher_settings_get_cache_expiration (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_cache_expiration (self->model->settings, values[selected], &error))
    {
      NuvashaLauncherSettingsCacheExpiration current =
        nuvashalauncher_settings_get_cache_expiration (self->model->settings);
      guint current_index = 0;

      for (guint i = 0; i < G_N_ELEMENTS (values); i++)
        if (values[i] == current)
          current_index = i;
      adw_combo_row_set_selected (row, current_index);
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_clear_cache_confirmed (GObject *source, GAsyncResult *result,
                          gpointer user_data);

static void
on_clear_cache_clicked (GtkButton *button G_GNUC_UNUSED,
                        gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
     adw_alert_dialog_new (_("Clear cache?"), NULL));

  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "clear", _("Clear"));
  adw_alert_dialog_set_response_appearance (alert, "clear",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_clear_cache_confirmed, g_object_ref (self));
}

typedef struct
{
  NuvashaLauncherMainWindow *window;
  NuvashaLauncherTaskScope *operation;
} CacheClearWait;

static void
on_cache_cleared (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  CacheClearWait *wait = user_data;
  NuvashaLauncherMainWindow *self = wait->window;
  g_autoptr (GError) error = NULL;
  gboolean success = g_task_propagate_boolean (G_TASK (result), &error);

  if (success)
    {
      if (self->ui.toast_overlay != NULL)
        adw_toast_overlay_add_toast (self->ui.toast_overlay,
                                     adw_toast_new (_("Cache cleared")));
    }
  else
    nuvashalauncher_ui_show_error (GTK_WIDGET (self), _("Cache cleanup failed"), error);

  g_object_unref (self);
  nuvashalauncher_task_scope_unref (wait->operation);
  g_free (wait);
}

static void
on_clear_cache_confirmed (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "clear") != 0)
    {
      g_object_unref (self);
      return;
    }

  CacheClearWait *wait = g_new0 (CacheClearWait, 1);
  wait->window = self;
  wait->operation = nuvashalauncher_cache_clear_all_async (on_cache_cleared, wait);
}

static void
on_default_memory_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSpinRow *row = ADW_SPIN_ROW (object);
  guint value = (guint) adw_spin_row_get_value (row);
  g_autoptr (GError) error = NULL;

  if (value == nuvashalauncher_settings_get_default_memory_mib (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_default_memory_mib (self->model->settings, value, &error))
    {
      adw_spin_row_set_value (
        row, nuvashalauncher_settings_get_default_memory_mib (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_default_jvm_args_apply (AdwEntryRow *row, gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (row));
  g_autoptr (GError) error = NULL;

  if (g_strcmp0 (text, nuvashalauncher_settings_get_default_jvm_args (self->model->settings)) == 0)
    return;

  if (!nuvashalauncher_settings_set_default_jvm_args (self->model->settings, text, &error))
    {
      gtk_editable_set_text (
        GTK_EDITABLE (row),
        nuvashalauncher_settings_get_default_jvm_args (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_default_game_width_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                               gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSpinRow *row = ADW_SPIN_ROW (object);
  guint value = (guint) adw_spin_row_get_value (row);
  g_autoptr (GError) error = NULL;

  if (value == nuvashalauncher_settings_get_default_game_width (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_default_game_width (self->model->settings, value, &error))
    {
      adw_spin_row_set_value (
        row, nuvashalauncher_settings_get_default_game_width (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_default_game_height_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                                gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSpinRow *row = ADW_SPIN_ROW (object);
  guint value = (guint) adw_spin_row_get_value (row);
  g_autoptr (GError) error = NULL;

  if (value == nuvashalauncher_settings_get_default_game_height (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_default_game_height (self->model->settings, value, &error))
    {
      adw_spin_row_set_value (
        row, nuvashalauncher_settings_get_default_game_height (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_default_game_fullscreen_toggled (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                                    gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_default_game_fullscreen (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_default_game_fullscreen (self->model->settings, active, &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_default_game_fullscreen (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_default_use_dedicated_gpu_toggled (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                                      gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_default_use_dedicated_gpu (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_default_use_dedicated_gpu (self->model->settings, active, &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_default_use_dedicated_gpu (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_default_use_gamemode_toggled (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                                 gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_default_use_gamemode (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_default_use_gamemode (self->model->settings, active, &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_default_use_gamemode (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static void
on_default_use_mangohud_toggled (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                                 gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  g_autoptr (GError) error = NULL;

  if (active == nuvashalauncher_settings_get_default_use_mangohud (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_default_use_mangohud (self->model->settings, active, &error))
    {
      adw_switch_row_set_active (
        row, nuvashalauncher_settings_get_default_use_mangohud (self->model->settings));
      nuvashalauncher_ui_show_error (GTK_WIDGET (row), _("Technical details"), error);
    }
}

static AdwDialog *
create_settings_dialog (NuvashaLauncherMainWindow *self)
{
  AdwDialog *dialog = adw_preferences_dialog_new ();
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  AdwPreferencesGroup *versions_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *logs_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *language_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *cache_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *general_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *defaults_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwComboRow *language_row = ADW_COMBO_ROW (adw_combo_row_new ());
  AdwSwitchRow *row = ADW_SWITCH_ROW (adw_switch_row_new ());
  AdwSpinRow *limit_row = NULL;
  AdwSwitchRow *unstable_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  AdwSwitchRow *cache_api_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  AdwComboRow *cache_expiration_row = ADW_COMBO_ROW (adw_combo_row_new ());
  AdwActionRow *clear_cache_row = ADW_ACTION_ROW (adw_action_row_new ());
  AdwSwitchRow *export_metadata_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  AdwSwitchRow *delete_dependents_row = ADW_SWITCH_ROW (adw_switch_row_new ());

   /* Bound the settings surface to a usable width and a scrollable
    * maximum-height-sized page. */
  adw_dialog_set_content_width (dialog, 640);
  adw_dialog_set_content_height (dialog, 580);

  /* Every one of these rows can show a path or an error message from
     GIO, which is plain text and may hold an "&". */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (unstable_row),
                                      FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (cache_api_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (cache_expiration_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (clear_cache_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (export_metadata_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (delete_dependents_row), FALSE);

  adw_dialog_set_title (dialog, _("Settings"));
  adw_preferences_dialog_set_search_enabled (ADW_PREFERENCES_DIALOG (dialog),
                                             FALSE);

  adw_preferences_group_set_title (versions_group,
                                     _("Versions"));
  adw_preferences_group_set_title (general_group,
                                    _("General"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (export_metadata_row),
                                  _("Ask for export metadata"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (export_metadata_row),
                               _("Show name, summary and version fields before exporting."));
  adw_switch_row_set_active (ADW_SWITCH_ROW (export_metadata_row),
                             nuvashalauncher_settings_get_export_metadata (self->model->settings));
    g_signal_connect_object (export_metadata_row, "notify::active",
                             G_CALLBACK (on_export_metadata_toggled), self, 0);
   adw_preferences_group_add (general_group,
                                GTK_WIDGET (export_metadata_row));

   adw_preferences_row_set_title (
     ADW_PREFERENCES_ROW (delete_dependents_row),
     _("Deleting a dependency deletes the mod that depended on it"));
   adw_switch_row_set_active (
     delete_dependents_row,
     nuvashalauncher_settings_get_delete_dependents (self->model->settings));
    g_signal_connect_object (delete_dependents_row, "notify::active",
                             G_CALLBACK (on_delete_dependents_toggled), self, 0);
   adw_preferences_group_add (general_group,
                                 GTK_WIDGET (delete_dependents_row));


   adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                  _("Show unstable Minecraft versions"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                             nuvashalauncher_settings_get_show_unstable_versions (
                               self->model->settings));

  /* Connected after the value was set, so opening the dialog never
   * writes anything by itself. */
   g_signal_connect_object (row, "notify::active",
                             G_CALLBACK (on_show_unstable_versions_toggled), self, 0);
  adw_preferences_group_add (versions_group, GTK_WIDGET (row));

  /* The loader versions live under the same choice: both say what the
     version lists hold. */
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (unstable_row),
                                  _("Show unstable mod loader versions"));
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (unstable_row),
    nuvashalauncher_settings_get_show_unstable_loaders (self->model->settings));
   g_signal_connect_object (unstable_row, "notify::active",
                            G_CALLBACK (on_show_unstable_loaders_toggled), self, 0);
  adw_preferences_group_add (versions_group,
                              GTK_WIDGET (unstable_row));

  /* Logs */
  adw_preferences_group_set_title (logs_group,
                                   _("Logs"));

  {
    gsize length = 0;
    const NuvashaLauncherLanguage *languages = nuvashalauncher_i18n_languages (&length);
    GtkStringList *model = gtk_string_list_new (NULL);
    guint selected = 0;
    const char *current = nuvashalauncher_settings_get_language (self->model->settings);

    for (gsize i = 0; i < length; i++)
      {
        gtk_string_list_append (model, _(languages[i].name));
        if (g_strcmp0 (current, languages[i].code) == 0)
          selected = (guint) i;
      }

    adw_preferences_group_set_title (language_group,
                                     _("Language"));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (language_row),
                                   _("Interface language"));
    adw_combo_row_set_model (ADW_COMBO_ROW (language_row),
                             G_LIST_MODEL (model));
    adw_combo_row_set_selected (ADW_COMBO_ROW (language_row), selected);
     g_signal_connect_object (language_row, "notify::selected",
                              G_CALLBACK (on_language_selected), self, 0);
    adw_preferences_group_add (language_group,
                                GTK_WIDGET (language_row));
    g_object_unref (model);
  }

  limit_row = ADW_SPIN_ROW (adw_spin_row_new (
    gtk_adjustment_new (nuvashalauncher_settings_get_log_line_limit (self->model->settings), 100,
                        100000, 500, 5000, 0),
    0, 0));
  adw_spin_row_set_digits (limit_row, 0);
  adw_spin_row_set_numeric (limit_row, TRUE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (limit_row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (limit_row),
                                  _("Line limit"));
   g_signal_connect_object (limit_row, "notify::value",
                            G_CALLBACK (on_log_limit_changed), self, 0);
  adw_preferences_group_add (logs_group, GTK_WIDGET (limit_row));

  adw_preferences_group_set_title (cache_group,
                                    _("Cache"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cache_api_row),
                                  _("Cache Modrinth API responses"));
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (cache_api_row),
    nuvashalauncher_settings_get_cache_modrinth_api_responses (self->model->settings));
   g_signal_connect_object (cache_api_row, "notify::active",
                            G_CALLBACK (on_cache_modrinth_api_responses_toggled), self, 0);
  adw_preferences_group_add (cache_group, GTK_WIDGET (cache_api_row));

  {
    GtkStringList *model = gtk_string_list_new (NULL);
    const char *labels[] = { _("1 hour"), _("6 hours"), _("1 day"), _("7 days") };
    NuvashaLauncherSettingsCacheExpiration current =
      nuvashalauncher_settings_get_cache_expiration (self->model->settings);
    guint selected = 0;
    const NuvashaLauncherSettingsCacheExpiration values[] = {
      NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_1_HOUR,
      NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_6_HOURS,
      NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_1_DAY,
      NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_7_DAYS
    };

    for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
      {
        gtk_string_list_append (model, labels[i]);
        if (values[i] == current)
          selected = i;
      }
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cache_expiration_row),
                                    _("Cache expiration"));
    adw_combo_row_set_model (ADW_COMBO_ROW (cache_expiration_row),
                             G_LIST_MODEL (model));
    adw_combo_row_set_selected (ADW_COMBO_ROW (cache_expiration_row), selected);
     g_signal_connect_object (cache_expiration_row, "notify::selected",
                              G_CALLBACK (on_cache_expiration_selected), self, 0);
    adw_preferences_group_add (cache_group,
                                GTK_WIDGET (cache_expiration_row));
    g_object_unref (model);
  }

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (clear_cache_row),
                                   _("Clear cache"));
  {
    GtkButton *button = GTK_BUTTON (gtk_button_new_from_icon_name ("user-trash-symbolic"));
    gtk_widget_add_css_class (GTK_WIDGET (button), "destructive-action");
    nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (button), _("Clear cache"));
    g_signal_connect_object (button, "clicked", G_CALLBACK (on_clear_cache_clicked),
                             self, 0);
    adw_action_row_add_suffix (clear_cache_row, GTK_WIDGET (button));
  }
  adw_preferences_group_add (cache_group, GTK_WIDGET (clear_cache_row));

  /* Global launch defaults */
  adw_preferences_group_set_title (defaults_group, _("Launch defaults"));
  {
    AdwSpinRow *ram_row = ADW_SPIN_ROW (adw_spin_row_new (
      gtk_adjustment_new (
        nuvashalauncher_settings_get_default_memory_mib (self->model->settings),
        512, 65536, 512, 2048, 0),
      0, 0));
    adw_spin_row_set_digits (ram_row, 0);
    adw_spin_row_set_numeric (ram_row, TRUE);
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (ram_row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ram_row), _("RAM (MiB)"));
    g_signal_connect_object (ram_row, "notify::value",
                             G_CALLBACK (on_default_memory_changed), self, 0);
    adw_preferences_group_add (defaults_group, GTK_WIDGET (ram_row));

    AdwEntryRow *jvm_row = ADW_ENTRY_ROW (adw_entry_row_new ());
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (jvm_row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (jvm_row), _("JVM Arguments"));
    adw_entry_row_set_show_apply_button (jvm_row, TRUE);
    gtk_editable_set_text (
      GTK_EDITABLE (jvm_row),
      nuvashalauncher_settings_get_default_jvm_args (self->model->settings));
    g_signal_connect_object (jvm_row, "apply",
                             G_CALLBACK (on_default_jvm_args_apply), self, 0);
    adw_preferences_group_add (defaults_group, GTK_WIDGET (jvm_row));

    AdwSpinRow *width_row = ADW_SPIN_ROW (adw_spin_row_new (
      gtk_adjustment_new (
        nuvashalauncher_settings_get_default_game_width (self->model->settings),
        320, 7680, 10, 100, 0),
      0, 0));
    adw_spin_row_set_digits (width_row, 0);
    adw_spin_row_set_numeric (width_row, TRUE);
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (width_row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (width_row), _("Window width"));
    g_signal_connect_object (width_row, "notify::value",
                             G_CALLBACK (on_default_game_width_changed), self, 0);
    adw_preferences_group_add (defaults_group, GTK_WIDGET (width_row));

    AdwSpinRow *height_row = ADW_SPIN_ROW (adw_spin_row_new (
      gtk_adjustment_new (
        nuvashalauncher_settings_get_default_game_height (self->model->settings),
        240, 4320, 10, 100, 0),
      0, 0));
    adw_spin_row_set_digits (height_row, 0);
    adw_spin_row_set_numeric (height_row, TRUE);
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (height_row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (height_row), _("Window height"));
    g_signal_connect_object (height_row, "notify::value",
                             G_CALLBACK (on_default_game_height_changed), self, 0);
    adw_preferences_group_add (defaults_group, GTK_WIDGET (height_row));

    AdwSwitchRow *fullscreen_row = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (fullscreen_row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (fullscreen_row),
                                   _("Start in fullscreen"));
    adw_switch_row_set_active (
      fullscreen_row,
      nuvashalauncher_settings_get_default_game_fullscreen (self->model->settings));
    g_signal_connect_object (fullscreen_row, "notify::active",
                             G_CALLBACK (on_default_game_fullscreen_toggled), self, 0);
    adw_preferences_group_add (defaults_group, GTK_WIDGET (fullscreen_row));

    AdwSwitchRow *gpu_row = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (gpu_row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (gpu_row),
                                   _("Use dedicated GPU"));
    adw_switch_row_set_active (
      gpu_row,
      nuvashalauncher_settings_get_default_use_dedicated_gpu (self->model->settings));
    g_signal_connect_object (gpu_row, "notify::active",
                             G_CALLBACK (on_default_use_dedicated_gpu_toggled), self, 0);
    adw_preferences_group_add (defaults_group, GTK_WIDGET (gpu_row));

    {
      g_autofree char *gamemode_bin = g_find_program_in_path ("gamemoderun");
      gboolean gamemode_active =
        nuvashalauncher_settings_get_default_use_gamemode (self->model->settings);
      AdwSwitchRow *gamemode_row = ADW_SWITCH_ROW (adw_switch_row_new ());

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (gamemode_row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (gamemode_row),
                                     _("Use GameMode"));
      if (gamemode_bin == NULL)
        {
          adw_action_row_set_subtitle (ADW_ACTION_ROW (gamemode_row), _("Not installed"));
          if (!gamemode_active)
            gtk_widget_set_sensitive (GTK_WIDGET (gamemode_row), FALSE);
        }
      adw_switch_row_set_active (gamemode_row, gamemode_active);
      g_signal_connect_object (gamemode_row, "notify::active",
                               G_CALLBACK (on_default_use_gamemode_toggled), self, 0);
      adw_preferences_group_add (defaults_group, GTK_WIDGET (gamemode_row));
    }

    {
      g_autofree char *mangohud_bin = g_find_program_in_path ("mangohud");
      gboolean mangohud_active =
        nuvashalauncher_settings_get_default_use_mangohud (self->model->settings);
      AdwSwitchRow *mangohud_row = ADW_SWITCH_ROW (adw_switch_row_new ());

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (mangohud_row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (mangohud_row),
                                     _("Use MangoHud"));
      if (mangohud_bin == NULL)
        {
          adw_action_row_set_subtitle (ADW_ACTION_ROW (mangohud_row), _("Not installed"));
          if (!mangohud_active)
            gtk_widget_set_sensitive (GTK_WIDGET (mangohud_row), FALSE);
        }
      adw_switch_row_set_active (mangohud_row, mangohud_active);
      g_signal_connect_object (mangohud_row, "notify::active",
                               G_CALLBACK (on_default_use_mangohud_toggled), self, 0);
      adw_preferences_group_add (defaults_group, GTK_WIDGET (mangohud_row));
    }
  }

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             general_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             defaults_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             versions_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             logs_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             language_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             cache_group);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog),
                              ADW_PREFERENCES_PAGE (page));

  return dialog;
}

static void
on_settings (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwDialog *dialog = create_settings_dialog (self);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_about (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwDialog *dialog = adw_about_dialog_new ();
  const char *authors[] = { "aKpobzwviq", NULL };
  const char *services[] = {
    "Modrinth API",
    "Mojang & Microsoft Services",
    "FabricMC",
    "QuiltMC",
    "Minecraft Forge",
    "NeoForged",
    NULL
  };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog),
                                         _("Nuvasha Launcher"));
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (dialog),
                                         "com.github.NuvashaLauncher.NuvashaLauncher");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), NUVASHALAUNCHER_VERSION);
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "aKpobzwviq");
  adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (dialog), authors);
  adw_about_dialog_set_designers (ADW_ABOUT_DIALOG (dialog), authors);
  adw_about_dialog_set_artists (ADW_ABOUT_DIALOG (dialog), authors);
  adw_about_dialog_set_translator_credits (ADW_ABOUT_DIALOG (dialog), _("translator-credits"));
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (dialog),
                                 _("Minecraft launcher for Linux"));
  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog),
                                "https://github.com/NuvashaLauncher/NuvashaLauncher");
  adw_about_dialog_set_issue_url (ADW_ABOUT_DIALOG (dialog),
                                  "https://github.com/NuvashaLauncher/NuvashaLauncher/issues");
  adw_about_dialog_set_support_url (ADW_ABOUT_DIALOG (dialog),
                                   "https://github.com/NuvashaLauncher/NuvashaLauncher#readme");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog),
                                  "© 2026 aKpobzwviq");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog),
                                     GTK_LICENSE_GPL_3_0_ONLY);

  adw_about_dialog_add_credit_section (ADW_ABOUT_DIALOG (dialog),
                                       _("Services & ecosystem"), services);

  adw_about_dialog_add_legal_section (
    ADW_ABOUT_DIALOG (dialog),
    _("Minecraft disclaimer"),
    NULL,
    GTK_LICENSE_CUSTOM,
    _("NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.\n\n"
      "Minecraft is a trademark of Mojang AB. The launcher is developed independently and is not affiliated with Mojang Studios or Microsoft."));

  adw_about_dialog_add_legal_section (
    ADW_ABOUT_DIALOG (dialog),
    _("Modrinth & mod loaders"),
    NULL,
    GTK_LICENSE_CUSTOM,
    _("Modpacks, mods, resource packs, and shaders are hosted by Modrinth and created by their respective authors.\n\n"
      "Mod loaders are developed independently by the Fabric, Quilt, Forge, and NeoForge projects."));

  adw_about_dialog_add_legal_section (
    ADW_ABOUT_DIALOG (dialog),
    _("AppStream metadata"),
    "© 2026 aKpobzwviq",
    GTK_LICENSE_CUSTOM,
    _("The AppStream metadata (descriptions, summaries, and release notes) is released under the Creative Commons CC0 1.0 Universal Public Domain Dedication."));

  adw_dialog_set_title (dialog, _("About Nuvasha Launcher"));

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* The shortcuts the window answers to, in the list the Shortcuts dialog
 * shows and as the accelerators registered at the end of init. */
static void
on_shortcuts (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  AdwDialog *dialog = adw_shortcuts_dialog_new ();
  AdwShortcutsSection *general = adw_shortcuts_section_new (_("General"));

  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Keyboard shortcuts"), "<primary>question"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Create instance"), "<primary>n"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Preferences"), "<primary>comma"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Quit"), "<primary>q"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Delete selected"), "Delete"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Search"), "<primary>f"));

  adw_shortcuts_dialog_add (ADW_SHORTCUTS_DIALOG (dialog), general);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

const GActionEntry window_entries[] = {
  { .name = "create-instance", .activate = on_create_instance },
  { .name = "download-modpack", .activate = on_download_modpack },
  { .name = "import-modpack", .activate = on_import_modpack },
  { .name = "settings", .activate = on_settings },
  { .name = "shortcuts", .activate = on_shortcuts },
  { .name = "about", .activate = on_about },
};

GMenu *
create_add_menu (void)
{
  GMenu *menu = g_menu_new ();

  g_menu_append (menu, _("Create instance"), "win.create-instance");
  g_menu_append (menu, _("Download modpack"), "win.download-modpack");
  g_menu_append (menu, _("Import modpack…"), "win.import-modpack");

  return menu;
}

GMenu *
create_app_menu (void)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) general = g_menu_new ();
  g_autoptr (GMenu) rest = g_menu_new ();

  g_menu_append (general, _("Preferences"), "win.settings");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (general));

  g_menu_append (rest, _("Keyboard shortcuts"), "win.shortcuts");
  g_menu_append (rest, _("About Nuvasha Launcher"), "win.about");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (rest));

  return g_steal_pointer (&menu);
}

/* GtkPopoverMenu builds its item widgets itself and ignores any
 * "tooltip-text" item attribute, so the tooltip of an entry is attached
 * to the item widget after the popover was built. */
void
set_menu_item_tooltip (GtkWidget  *root,
                       const char *label_text,
                       const char *tooltip)
{
  for (GtkWidget *child = gtk_widget_get_first_child (root);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      if (GTK_IS_LABEL (child) &&
          g_strcmp0 (gtk_label_get_text (GTK_LABEL (child)), label_text) == 0)
        {
          /* The label sits directly inside the menu item. */
          GtkWidget *item = gtk_widget_get_parent (child);

          if (item != NULL)
            gtk_widget_set_tooltip_text (item, tooltip);

          return;
        }

      set_menu_item_tooltip (child, label_text, tooltip);
    }
}

const guint window_entries_count = G_N_ELEMENTS (window_entries);
