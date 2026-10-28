/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-create-instance-dialog.h"
#include "nuvashalauncher-download-modpack-dialog.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-operation.h"

static void
on_create_instance (GSimpleAction *action G_GNUC_UNUSED,
                    GVariant      *parameter G_GNUC_UNUSED,
                    gpointer       user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  nuvashalauncherCreateInstanceDialog *dialog =
    nuvashalauncher_create_instance_dialog_new (self->model->manager, self->model->settings);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_download_modpack (GSimpleAction *action G_GNUC_UNUSED,
                     GVariant      *parameter G_GNUC_UNUSED,
                     gpointer       user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  nuvashalauncherDownloadModpackDialog *dialog =
    nuvashalauncher_download_modpack_dialog_new (self->model->manager);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_import_modpack_chosen (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
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
        gtk_widget_set_tooltip_text (GTK_WIDGET (self), error->message);
      g_object_unref (self);
      return;
    }

  {
    g_autofree char *path = g_file_get_path (file);

    if (path == NULL)
      {
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
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  gtk_file_dialog_set_title (dialog, _("Import Modpack"));
  gtk_file_filter_set_name (filter, _("Modrinth Modpacks (*.mrpack)"));
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
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
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
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), error->message);
}

/* Written the moment the value changes. A spin row can produce a value
 * for every click of its arrows, so the number that is already stored is
 * skipped rather than written over and over. */
static void
on_log_limit_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwSpinRow *row = ADW_SPIN_ROW (object);
  guint limit = (guint) adw_spin_row_get_value (row);

  if (limit == nuvashalauncher_settings_get_log_line_limit (self->model->settings))
    return;

  /* A limit that cannot be written leaves the row showing the number the
     settings file holds. */
  if (!nuvashalauncher_settings_set_log_line_limit (self->model->settings, limit, NULL))
    adw_spin_row_set_value (
      row, nuvashalauncher_settings_get_log_line_limit (self->model->settings));
}

/* Same contract as the snapshots switch: written at once, and put back
   what the file says when the file says no. */
static void
on_show_unstable_loaders_toggled (GObject *object,
                                  GParamSpec *pspec G_GNUC_UNUSED,
                                  gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == nuvashalauncher_settings_get_show_unstable_loaders (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_show_unstable_loaders (self->model->settings, active, NULL))
    adw_switch_row_set_active (
      row, nuvashalauncher_settings_get_show_unstable_loaders (self->model->settings));
}

static void
on_export_metadata_toggled (GObject *object,
                            GParamSpec *pspec G_GNUC_UNUSED,
                            gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == nuvashalauncher_settings_get_export_metadata (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_export_metadata (self->model->settings, active, NULL))
    adw_switch_row_set_active (
      row, nuvashalauncher_settings_get_export_metadata (self->model->settings));
}

static void
on_delete_dependents_toggled (GObject *object,
                               GParamSpec *pspec G_GNUC_UNUSED,
                               gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == nuvashalauncher_settings_get_delete_dependents (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_delete_dependents (self->model->settings, active, NULL))
    adw_switch_row_set_active (
      row, nuvashalauncher_settings_get_delete_dependents (self->model->settings));
}

static void
on_language_selected (GObject *object,
                      GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwComboRow *row = ADW_COMBO_ROW (object);
  gsize length = 0;
  const nuvashalauncherLanguage *languages = nuvashalauncher_i18n_languages (&length);
  guint selected = adw_combo_row_get_selected (row);

  if (selected >= length)
    return;

  nuvashalauncher_settings_set_language (self->model->settings, languages[selected].code, NULL);
}

static void
on_cache_modrinth_api_responses_toggled (GObject *object,
                                         GParamSpec *pspec G_GNUC_UNUSED,
                                         gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == nuvashalauncher_settings_get_cache_modrinth_api_responses (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_cache_modrinth_api_responses (self->model->settings, active,
                                                      NULL))
    adw_switch_row_set_active (
      row, nuvashalauncher_settings_get_cache_modrinth_api_responses (self->model->settings));
}

static void
on_cache_expiration_selected (GObject *object,
                               GParamSpec *pspec G_GNUC_UNUSED,
                               gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwComboRow *row = ADW_COMBO_ROW (object);
  static const nuvashalauncherSettingsCacheExpiration values[] = {
    nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_HOUR,
    nuvashalauncher_SETTINGS_CACHE_EXPIRATION_6_HOURS,
    nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_DAY,
    nuvashalauncher_SETTINGS_CACHE_EXPIRATION_7_DAYS
  };
  guint selected = adw_combo_row_get_selected (row);

  if (selected >= G_N_ELEMENTS (values) ||
      values[selected] == nuvashalauncher_settings_get_cache_expiration (self->model->settings))
    return;

  if (!nuvashalauncher_settings_set_cache_expiration (self->model->settings, values[selected], NULL))
    {
      nuvashalauncherSettingsCacheExpiration current =
        nuvashalauncher_settings_get_cache_expiration (self->model->settings);
      guint current_index = 0;

      for (guint i = 0; i < G_N_ELEMENTS (values); i++)
        if (values[i] == current)
          current_index = i;
      adw_combo_row_set_selected (row, current_index);
    }
}

static void
on_clear_cache_confirmed (GObject *source, GAsyncResult *result,
                          gpointer user_data);

static void
on_clear_cache_clicked (GtkButton *button G_GNUC_UNUSED,
                        gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
     adw_alert_dialog_new (_("Clear Nuvasha Launcher Cache?"), NULL));

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
  nuvashalauncherMainWindow *window;
  nuvashalauncherTaskScope *operation;
} CacheClearWait;

static void
on_cache_cleared (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  CacheClearWait *wait = user_data;
  nuvashalauncherMainWindow *self = wait->window;
  g_autoptr (GError) error = NULL;
  gboolean success = g_task_propagate_boolean (G_TASK (result), &error);
  AdwAlertDialog *alert = NULL;

  if (success)
    alert = ADW_ALERT_DIALOG (
      adw_alert_dialog_new (_("Cache Cleared"),
                             _("Nuvasha Launcher cache data was cleared successfully.")));
  else
    alert = ADW_ALERT_DIALOG (
      adw_alert_dialog_new (_("Cache Cleanup Failed"),
                            error != NULL ? error->message
                                          : _("The cache could not be cleared.")));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "ok", _("OK"));
  adw_alert_dialog_set_default_response (alert, "ok");
  adw_alert_dialog_set_close_response (alert, "ok");
  adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
  g_object_unref (self);
  nuvashalauncher_operation_unref (wait->operation);
  g_free (wait);
}

static void
on_clear_cache_confirmed (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
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

static AdwDialog *
create_settings_dialog (nuvashalauncherMainWindow *self)
{
  AdwDialog *dialog = adw_preferences_dialog_new ();
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  AdwPreferencesGroup *versions_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *logs_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *language_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *cache_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *general_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
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
                                  _("Show unstable Mod Loader versions"));
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
    const nuvashalauncherLanguage *languages = nuvashalauncher_i18n_languages (&length);
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
                                  _("Line Limit"));
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
    nuvashalauncherSettingsCacheExpiration current =
      nuvashalauncher_settings_get_cache_expiration (self->model->settings);
    guint selected = 0;
    const nuvashalauncherSettingsCacheExpiration values[] = {
      nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_HOUR,
      nuvashalauncher_SETTINGS_CACHE_EXPIRATION_6_HOURS,
      nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_DAY,
      nuvashalauncher_SETTINGS_CACHE_EXPIRATION_7_DAYS
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
                                   _("Clear Nuvasha Launcher Cache"));
  {
    GtkButton *button = GTK_BUTTON (gtk_button_new_from_icon_name ("user-trash-symbolic"));
     nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (button), _("Clear Nuvasha Launcher Cache"));
     g_signal_connect_object (button, "clicked", G_CALLBACK (on_clear_cache_clicked),
                              self, 0);
    adw_action_row_add_suffix (clear_cache_row, GTK_WIDGET (button));
  }
  adw_preferences_group_add (cache_group, GTK_WIDGET (clear_cache_row));

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             general_group);
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
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwDialog *dialog = create_settings_dialog (self);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_about (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwDialog *dialog = adw_about_dialog_new ();
  const char *app_ids[] = { "com.github.nuvashalauncher.nuvashalauncher", NULL };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog),
                                         "nuvashalauncher");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), nuvashalauncher_VERSION);
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "TestUser");

  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog),
                                 "https://github.com/nuvashalauncher/nuvashalauncher");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog),
                                  "Copyright (C) 2026 Flia");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog),
                                     GTK_LICENSE_GPL_3_0_ONLY);
  adw_about_dialog_add_credit_section (ADW_ABOUT_DIALOG (dialog),
                                       "Application ID", app_ids);
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
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
  AdwDialog *dialog = adw_shortcuts_dialog_new ();
  AdwShortcutsSection *general = adw_shortcuts_section_new (_("General"));

  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Show Shortcuts"), "F1"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Create Instance"), "<primary>n"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Settings"), "<primary>comma"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new (_("Quit"), "<primary>q"));

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

  g_menu_append (menu, _("Create Instance"), "win.create-instance");
  g_menu_append (menu, _("Download Modpack"), "win.download-modpack");
  g_menu_append (menu, _("Import Modpack"), "win.import-modpack");

  return menu;
}

GMenu *
create_app_menu (void)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) general = g_menu_new ();
  g_autoptr (GMenu) rest = g_menu_new ();

  g_menu_append (general, _("Settings"), "win.settings");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (general));

  g_menu_append (rest, _("Keyboard Shortcuts"), "win.shortcuts");
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
