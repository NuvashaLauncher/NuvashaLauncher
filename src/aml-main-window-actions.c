/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-main-window-private.h"
#include "aml-cache.h"
#include "aml-create-instance-dialog.h"
#include "aml-download-modpack-dialog.h"
#include "aml-i18n.h"

static void
on_create_instance (GSimpleAction *action G_GNUC_UNUSED,
                    GVariant      *parameter G_GNUC_UNUSED,
                    gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  GtkWidget *dialog = GTK_WIDGET (
    aml_create_instance_dialog_new (self->model->manager, self->model->settings));

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_download_modpack (GSimpleAction *action G_GNUC_UNUSED,
                     GVariant      *parameter G_GNUC_UNUSED,
                     gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  GtkWidget *dialog = GTK_WIDGET (aml_download_modpack_dialog_new (
    self->model->manager));

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_import_modpack_chosen (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  AmlMainWindow *self = user_data;
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
        g_warning ("cannot import a non-local modpack file");
        g_object_unref (self);
        return;
      }

    GtkWidget *dialog = GTK_WIDGET (
      aml_download_modpack_dialog_new_for_file (self->model->manager, path));

    /* Import runs as a background install: its AmlDownload entry appears
     * beside the + button just like a downloaded modpack. */
    g_object_ref_sink (dialog);
    g_object_unref (dialog);
  }

  g_object_unref (self);
}

static void
on_import_modpack (GSimpleAction *action G_GNUC_UNUSED,
                   GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
  AmlMainWindow *self = user_data;
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
on_show_snapshots_toggled (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  gboolean stored = aml_settings_get_show_snapshots (self->model->settings);
  g_autoptr (GError) error = NULL;

  if (active == stored)
    return;

  if (aml_settings_set_show_snapshots (self->model->settings, active, &error))
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
  AmlMainWindow *self = user_data;
  AdwSpinRow *row = ADW_SPIN_ROW (object);
  guint limit = (guint) adw_spin_row_get_value (row);

  if (limit == aml_settings_get_log_line_limit (self->model->settings))
    return;

  /* A limit that cannot be written leaves the row showing the number the
     settings file holds. */
  if (!aml_settings_set_log_line_limit (self->model->settings, limit, NULL))
    adw_spin_row_set_value (
      row, aml_settings_get_log_line_limit (self->model->settings));
}

/* Same contract as the snapshots switch: written at once, and put back
   what the file says when the file says no. */
static void
on_show_unstable_loaders_toggled (GObject *object,
                                  GParamSpec *pspec G_GNUC_UNUSED,
                                  gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == aml_settings_get_show_unstable_loaders (self->model->settings))
    return;

  if (!aml_settings_set_show_unstable_loaders (self->model->settings, active, NULL))
    adw_switch_row_set_active (
      row, aml_settings_get_show_unstable_loaders (self->model->settings));
}

static void
on_open_logs_after_crash_toggled (GObject *object,
                                  GParamSpec *pspec G_GNUC_UNUSED,
                                  gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == aml_settings_get_open_logs_after_crash (self->model->settings))
    return;

  if (!aml_settings_set_open_logs_after_crash (self->model->settings, active, NULL))
    adw_switch_row_set_active (
      row, aml_settings_get_open_logs_after_crash (self->model->settings));
}

static void
on_export_metadata_toggled (GObject *object,
                            GParamSpec *pspec G_GNUC_UNUSED,
                            gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == aml_settings_get_export_metadata (self->model->settings))
    return;

  if (!aml_settings_set_export_metadata (self->model->settings, active, NULL))
    adw_switch_row_set_active (
      row, aml_settings_get_export_metadata (self->model->settings));
}

static void
on_language_selected (GObject *object,
                      GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwComboRow *row = ADW_COMBO_ROW (object);
  gsize length = 0;
  const AmlLanguage *languages = aml_i18n_languages (&length);
  guint selected = adw_combo_row_get_selected (row);

  if (selected >= length)
    return;

  aml_settings_set_language (self->model->settings, languages[selected].code, NULL);
}

static void
on_cache_modrinth_api_responses_toggled (GObject *object,
                                         GParamSpec *pspec G_GNUC_UNUSED,
                                         gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == aml_settings_get_cache_modrinth_api_responses (self->model->settings))
    return;

  if (!aml_settings_set_cache_modrinth_api_responses (self->model->settings, active,
                                                      NULL))
    adw_switch_row_set_active (
      row, aml_settings_get_cache_modrinth_api_responses (self->model->settings));
}

static void
on_cache_expiration_selected (GObject *object,
                               GParamSpec *pspec G_GNUC_UNUSED,
                               gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwComboRow *row = ADW_COMBO_ROW (object);
  static const AmlSettingsCacheExpiration values[] = {
    AML_SETTINGS_CACHE_EXPIRATION_1_HOUR,
    AML_SETTINGS_CACHE_EXPIRATION_6_HOURS,
    AML_SETTINGS_CACHE_EXPIRATION_1_DAY,
    AML_SETTINGS_CACHE_EXPIRATION_7_DAYS
  };
  guint selected = adw_combo_row_get_selected (row);

  if (selected >= G_N_ELEMENTS (values) ||
      values[selected] == aml_settings_get_cache_expiration (self->model->settings))
    return;

  if (!aml_settings_set_cache_expiration (self->model->settings, values[selected], NULL))
    {
      AmlSettingsCacheExpiration current =
        aml_settings_get_cache_expiration (self->model->settings);
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
  AmlMainWindow *self = user_data;
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (_("Clear AML Cache?"), NULL));

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

static void
on_cache_cleared (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  AmlMainWindow *self = user_data;
  g_autoptr (GError) error = NULL;
  gboolean success = g_task_propagate_boolean (G_TASK (result), &error);
  AdwAlertDialog *alert = NULL;

  if (success)
    alert = ADW_ALERT_DIALOG (
      adw_alert_dialog_new ("Cache Cleared",
                            "AML cache data was cleared successfully."));
  else
    alert = ADW_ALERT_DIALOG (
      adw_alert_dialog_new ("Cache Cleanup Failed",
                            error != NULL ? error->message
                                          : "The cache could not be cleared."));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "ok", "OK");
  adw_alert_dialog_set_default_response (alert, "ok");
  adw_alert_dialog_set_close_response (alert, "ok");
  adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
  g_object_unref (self);
}

static void
on_clear_cache_confirmed (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  AmlMainWindow *self = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "clear") != 0)
    {
      g_object_unref (self);
      return;
    }

  aml_cache_clear_all_async (NULL, on_cache_cleared, self);
}

static void
on_settings (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  AdwDialog *dialog = adw_preferences_dialog_new ();
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *versions_group = adw_preferences_group_new ();
  GtkWidget *logs_group = adw_preferences_group_new ();
  GtkWidget *language_group = adw_preferences_group_new ();
  GtkWidget *cache_group = adw_preferences_group_new ();
  GtkWidget *general_group = adw_preferences_group_new ();
  GtkWidget *language_row = adw_combo_row_new ();
  GtkWidget *row = adw_switch_row_new ();
  GtkWidget *limit_row = NULL;
  GtkWidget *unstable_row = adw_switch_row_new ();
  GtkWidget *crash_row = adw_switch_row_new ();
  GtkWidget *cache_api_row = adw_switch_row_new ();
  GtkWidget *cache_expiration_row = adw_combo_row_new ();
  GtkWidget *clear_cache_row = adw_action_row_new ();
  GtkWidget *export_metadata_row = adw_switch_row_new ();

  /* Every one of these rows can show a path or an error message from
     GIO, which is plain text and may hold an "&". */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (unstable_row),
                                      FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (crash_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (cache_api_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (cache_expiration_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (clear_cache_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (export_metadata_row), FALSE);

  adw_dialog_set_title (dialog, _("Settings"));
  adw_preferences_dialog_set_search_enabled (ADW_PREFERENCES_DIALOG (dialog),
                                             FALSE);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (versions_group),
                                     _("Versions"));
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (general_group),
                                   _("General"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (export_metadata_row),
                                  _("Ask for export metadata"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (export_metadata_row),
                               _("Show name, summary and version fields before exporting."));
  adw_switch_row_set_active (ADW_SWITCH_ROW (export_metadata_row),
                             aml_settings_get_export_metadata (self->model->settings));
  g_signal_connect (export_metadata_row, "notify::active",
                    G_CALLBACK (on_export_metadata_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (general_group),
                              export_metadata_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 _("Show Snapshots"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                             aml_settings_get_show_snapshots (self->model->settings));

  /* Connected after the value was set, so opening the dialog never
   * writes anything by itself. */
  g_signal_connect (row, "notify::active",
                    G_CALLBACK (on_show_snapshots_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (versions_group), row);

  /* The loader versions live under the same choice: both say what the
     version lists hold. */
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (unstable_row),
                                  _("Show unstable Mod Loader versions"));
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (unstable_row),
    aml_settings_get_show_unstable_loaders (self->model->settings));
  g_signal_connect (unstable_row, "notify::active",
                    G_CALLBACK (on_show_unstable_loaders_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (versions_group),
                             unstable_row);

  /* Logs */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (logs_group),
                                   _("Logs"));

  {
    gsize length = 0;
    const AmlLanguage *languages = aml_i18n_languages (&length);
    GtkStringList *model = gtk_string_list_new (NULL);
    guint selected = 0;
    const char *current = aml_settings_get_language (self->model->settings);

    for (gsize i = 0; i < length; i++)
      {
        gtk_string_list_append (model, _(languages[i].name));
        if (g_strcmp0 (current, languages[i].code) == 0)
          selected = (guint) i;
      }

    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (language_group),
                                     _("Language"));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (language_row),
                                   _("Interface language"));
    adw_combo_row_set_model (ADW_COMBO_ROW (language_row),
                             G_LIST_MODEL (model));
    adw_combo_row_set_selected (ADW_COMBO_ROW (language_row), selected);
    g_signal_connect (language_row, "notify::selected",
                      G_CALLBACK (on_language_selected), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (language_group),
                               language_row);
    g_object_unref (model);
  }

  limit_row = adw_spin_row_new (
    gtk_adjustment_new (aml_settings_get_log_line_limit (self->model->settings), 100,
                        100000, 500, 5000, 0),
    0, 0);
  adw_spin_row_set_digits (ADW_SPIN_ROW (limit_row), 0);
  adw_spin_row_set_numeric (ADW_SPIN_ROW (limit_row), TRUE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (limit_row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (limit_row),
                                  _("Line Limit"));
  g_signal_connect (limit_row, "notify::value",
                    G_CALLBACK (on_log_limit_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (logs_group), limit_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (crash_row),
                                  _("Open Log Folder After Crash"));
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (crash_row),
    aml_settings_get_open_logs_after_crash (self->model->settings));
  g_signal_connect (crash_row, "notify::active",
                    G_CALLBACK (on_open_logs_after_crash_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (logs_group), crash_row);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (cache_group),
                                    _("Cache"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cache_api_row),
                                  _("Cache Modrinth API responses"));
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (cache_api_row),
    aml_settings_get_cache_modrinth_api_responses (self->model->settings));
  g_signal_connect (cache_api_row, "notify::active",
                    G_CALLBACK (on_cache_modrinth_api_responses_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cache_group), cache_api_row);

  {
    GtkStringList *model = gtk_string_list_new (NULL);
    const char *labels[] = { _("1 hour"), _("6 hours"), _("1 day"), _("7 days") };
    AmlSettingsCacheExpiration current =
      aml_settings_get_cache_expiration (self->model->settings);
    guint selected = 0;
    const AmlSettingsCacheExpiration values[] = {
      AML_SETTINGS_CACHE_EXPIRATION_1_HOUR,
      AML_SETTINGS_CACHE_EXPIRATION_6_HOURS,
      AML_SETTINGS_CACHE_EXPIRATION_1_DAY,
      AML_SETTINGS_CACHE_EXPIRATION_7_DAYS
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
    g_signal_connect (cache_expiration_row, "notify::selected",
                      G_CALLBACK (on_cache_expiration_selected), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (cache_group),
                               cache_expiration_row);
    g_object_unref (model);
  }

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (clear_cache_row),
                                  _("Clear AML Cache"));
  {
    GtkWidget *button = gtk_button_new_from_icon_name ("user-trash-symbolic");
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_set_tooltip_text (button, _("Clear AML Cache"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _("Clear AML Cache"), -1);
    g_signal_connect (button, "clicked", G_CALLBACK (on_clear_cache_clicked),
                      self);
    adw_action_row_add_suffix (ADW_ACTION_ROW (clear_cache_row), button);
  }
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (cache_group), clear_cache_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             ADW_PREFERENCES_GROUP (general_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             ADW_PREFERENCES_GROUP (versions_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             ADW_PREFERENCES_GROUP (logs_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             ADW_PREFERENCES_GROUP (language_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             ADW_PREFERENCES_GROUP (cache_group));
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog),
                              ADW_PREFERENCES_PAGE (page));

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_about (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  AdwDialog *dialog = adw_about_dialog_new ();
  const char *app_ids[] = { "io.gitlab.anotherminecraftlauncher.AML", NULL };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), "AML");
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "Flia");

  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog),
                                "https://gitlab.com/anotherminecraftlauncher/aml");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog),
                                  "Copyright (C) 2026 Flia");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog),
                                     GTK_LICENSE_GPL_3_0_ONLY);
  adw_about_dialog_add_credit_section (ADW_ABOUT_DIALOG (dialog),
                                       "Application ID", app_ids);
  adw_dialog_set_title (dialog, _("About AML"));

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* The shortcuts the window answers to, in the list the Shortcuts dialog
 * shows and as the accelerators registered at the end of init. */
static void
on_shortcuts (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  AmlMainWindow *self = user_data;
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
  g_menu_append (rest, _("About AML"), "win.about");
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
