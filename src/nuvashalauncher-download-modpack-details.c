/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Modrinth modpack quick-install and version selection. */
#include "nuvashalauncher-download-modpack-dialog-private.h"
#include "nuvashalauncher-icon-load-private.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-task.h"
#include <json-glib/json-glib.h>

#define VERSION_PICKER_BATCH_SIZE 25

static void
version_row_label (NuvashaLauncherModrinthVersion *version, char *out, guint size)
{
  const char *name = version->version_number != NULL ? version->version_number : version->name;
  const char *game =
      version->game_versions != NULL && version->game_versions->len > 0
          ? g_ptr_array_index (version->game_versions, 0)
          : "?";
  const char *loader =
      version->loaders != NULL && version->loaders->len > 0
          ? g_ptr_array_index (version->loaders, 0)
          : "vanilla";

  g_snprintf (out, size, "%s · %s · %s", name, game, loader);
}

static gint
by_date_desc (gconstpointer a, gconstpointer b)
{
  const NuvashaLauncherModrinthVersion *va = *(const NuvashaLauncherModrinthVersion *const *) a;
  const NuvashaLauncherModrinthVersion *vb = *(const NuvashaLauncherModrinthVersion *const *) b;

  return g_strcmp0 (vb->date_published, va->date_published);
}

static gboolean
version_matches_filter_list (GPtrArray *haystack, GPtrArray *needles)
{
  if (needles == NULL || needles->len == 0)
    return TRUE;

  if (haystack == NULL || haystack->len == 0)
    return FALSE;

  for (guint i = 0; i < haystack->len; i++)
    {
      const char *item = g_ptr_array_index (haystack, i);

      for (guint j = 0; j < needles->len; j++)
        {
          const char *wanted = g_ptr_array_index (needles, j);

          if (g_strcmp0 (item, wanted) == 0)
            return TRUE;
        }
    }

  return FALSE;
}

static GPtrArray *
versions_of_answer (JsonNode *root, GPtrArray *filter_loaders,
                    GPtrArray *filter_versions, GError **error)
{
  JsonArray *array = NULL;
  GPtrArray *versions = NULL;

  if (!JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error, NUVASHALAUNCHER_MODRINTH_ERROR,
                           NUVASHALAUNCHER_MODRINTH_ERROR_PARSE,
                           "the answer is not a list of versions");
      return NULL;
    }

  versions = g_ptr_array_new_with_free_func ((GDestroyNotify)
    nuvashalauncher_modrinth_version_free);

  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *element = json_array_get_element (array, i);
      NuvashaLauncherModrinthVersion *version =
        nuvashalauncher_modrinth_version_from_node (element, NULL);

      if (version == NULL)
        continue;

      {
        gboolean is_a_pack = FALSE;

        for (guint j = 0;
             version->files != NULL && j < version->files->len;
             j++)
          {
            NuvashaLauncherModrinthFile *file = g_ptr_array_index (version->files, j);

            if (file->filename != NULL &&
                g_str_has_suffix (file->filename, ".mrpack"))
              is_a_pack = TRUE;
          }

        if (!is_a_pack ||
            !version_matches_filter_list (version->loaders, filter_loaders) ||
            !version_matches_filter_list (version->game_versions, filter_versions))
          {
            nuvashalauncher_modrinth_version_free (version);
            continue;
          }
      }

      g_ptr_array_add (versions, version);
    }

  g_ptr_array_sort (versions, by_date_desc);

  return versions;
}

typedef struct
{
  NuvashaLauncherDownloadModpackDialog *dialog;
  NuvashaLauncherTaskScope *operation;
} ProjectIconLoad;

static void
project_icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  ProjectIconLoad *load = user_data;
  NuvashaLauncherDownloadModpackDialog *self = load->dialog;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = nuvashalauncher_icon_load_finish (result, &error);

  if (path != NULL)
    {
      g_free (self->icon_path);
      self->icon_path = g_steal_pointer (&path);
    }

  g_clear_object (&load->dialog);
  if (load->operation != NULL)
    nuvashalauncher_task_scope_unref (load->operation);
  g_free (load);
}

typedef struct {
  NuvashaLauncherModrinthProject *project;
  NuvashaLauncherModrinthVersion *version;
} ModpackInstallConfirm;

static void
modpack_install_confirm_free (ModpackInstallConfirm *confirm)
{
  if (confirm == NULL)
    return;
  g_clear_pointer (&confirm->project, nuvashalauncher_modrinth_project_free);
  g_clear_pointer (&confirm->version, nuvashalauncher_modrinth_version_free);
  g_free (confirm);
}

static void
do_start_modpack_install (NuvashaLauncherDownloadModpackDialog *self,
                          NuvashaLauncherModrinthProject *project,
                          NuvashaLauncherModrinthVersion *version)
{
  g_clear_pointer (&self->project, nuvashalauncher_modrinth_project_free);
  self->project = nuvashalauncher_modrinth_project_copy (project);

  g_free (self->instance_name);
  self->instance_name = g_strdup (project->title);

  g_clear_pointer (&self->version, nuvashalauncher_modrinth_version_free);
  self->version = nuvashalauncher_modrinth_version_copy (version);

  g_clear_pointer (&self->icon_path, g_free);
  g_clear_pointer (&self->icon_url, g_free);
  if (project->icon_url != NULL && *project->icon_url != '\0')
    {
      ProjectIconLoad *load = g_new0 (ProjectIconLoad, 1);
      self->icon_url = g_strdup (project->icon_url);
      load->dialog = g_object_ref (self);
      load->operation = nuvashalauncher_icon_load_async (project->icon_url,
                                                         project_icon_ready, load);
    }

  if (dialog_start_features (self))
    {
      if (!self->closed)
        {
          self->closed = TRUE;
          adw_dialog_close (ADW_DIALOG (self));
        }
    }
}

static void
on_modpack_install_confirmed (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncherDownloadModpackDialog *self =
    NUVASHALAUNCHER_DOWNLOAD_MODPACK_DIALOG (user_data);
  ModpackInstallConfirm *confirm =
    g_object_get_data (G_OBJECT (source), "nuvashalauncher-modpack-confirm");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (confirm == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (g_strcmp0 (response, "install") == 0 && !self->closed)
    {
      do_start_modpack_install (self, confirm->project, confirm->version);
    }

  modpack_install_confirm_free (confirm);
  g_object_unref (self);
}

static void
start_modpack_install (NuvashaLauncherDownloadModpackDialog *self,
                       NuvashaLauncherModrinthProject *project,
                       NuvashaLauncherModrinthVersion *version)
{
  if (self == NULL || self->closed || project == NULL)
    return;

  const char *title = (project->title != NULL && *project->title != '\0')
                        ? project->title
                        : "";
  g_autofree char *heading =
    g_strdup_printf (_("%s “%s”"), _("Install"), title);

  AdwAlertDialog *alert =
    ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, NULL));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "install", _("Install"));
  adw_alert_dialog_set_response_appearance (alert, "install",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  ModpackInstallConfirm *confirm = g_new0 (ModpackInstallConfirm, 1);
  confirm->project = nuvashalauncher_modrinth_project_copy (project);
  confirm->version = version != NULL
                       ? nuvashalauncher_modrinth_version_copy (version)
                       : NULL;

  g_object_set_data (G_OBJECT (alert), "nuvashalauncher-modpack-confirm", confirm);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_modpack_install_confirmed, g_object_ref (self));
}

typedef struct {
  NuvashaLauncherDownloadModpackDialog *dialog;
  NuvashaLauncherModrinthProject *project;
  AdwDialog *picker;
  GCancellable *cancellable;
} ModpackVersionFetch;

static void
modpack_version_fetch_free (ModpackVersionFetch *fetch)
{
  if (fetch == NULL)
    return;
  g_clear_object (&fetch->dialog);
  g_clear_pointer (&fetch->project, nuvashalauncher_modrinth_project_free);
  g_clear_object (&fetch->picker);
  g_clear_object (&fetch->cancellable);
  g_free (fetch);
}

static void
on_modpack_quick_versions_ready (GObject *source G_GNUC_UNUSED,
                                 GAsyncResult *result,
                                 gpointer user_data)
{
  ModpackVersionFetch *fetch = user_data;
  NuvashaLauncherDownloadModpackDialog *self = fetch->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser =
    nuvashalauncher_download_json_finish (result, &error);

  if (self == NULL || self->closed || g_cancellable_is_cancelled (fetch->cancellable))
    {
      modpack_version_fetch_free (fetch);
      return;
    }

  if (error != NULL)
    {
      if (!nuvashalauncher_error_is_cancelled (error))
        push_failure (self, _("Could not load versions. Try again."), error, NULL);
      modpack_version_fetch_free (fetch);
      return;
    }

  g_autoptr (GPtrArray) versions =
    versions_of_answer (json_parser_get_root (parser),
                        self->filter_loaders, self->filter_versions, &error);
  if (versions == NULL)
    {
      push_failure (self, _("Could not load versions. Try again."), error, NULL);
      modpack_version_fetch_free (fetch);
      return;
    }

  if (versions->len == 0)
    {
      push_error (self, _("Could not load versions. Try again."),
                  _("The requested file, project, or compatible version was not found."),
                  NULL);
      modpack_version_fetch_free (fetch);
      return;
    }

  NuvashaLauncherModrinthVersion *latest = g_ptr_array_index (versions, 0);
  start_modpack_install (self, fetch->project, latest);
  modpack_version_fetch_free (fetch);
}

void
modpack_quick_install_project (NuvashaLauncherDownloadModpackDialog *self,
                               NuvashaLauncherModrinthProject *project)
{
  if (self == NULL || project == NULL || project->project_id == NULL)
    return;

  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);
  g_clear_object (&self->fetch);
  self->fetch = g_cancellable_new ();

  g_autofree char *escaped_id = g_uri_escape_string (project->project_id, NULL, FALSE);
  g_autofree char *url =
    g_strdup_printf (NUVASHALAUNCHER_MODRINTH_URL "/project/%s/version", escaped_id);

  ModpackVersionFetch *fetch = g_new0 (ModpackVersionFetch, 1);
  fetch->dialog = g_object_ref (self);
  fetch->project = nuvashalauncher_modrinth_project_copy (project);
  fetch->cancellable = g_object_ref (self->fetch);

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_json_cached_async (
      nuvashalauncher_downloads_get_service (), url,
      on_modpack_quick_versions_ready, fetch),
    self->fetch);
}

typedef struct {
  GPtrArray *versions;
  guint loaded;
  AdwPreferencesGroup *group;
  AdwDialog *picker;
  NuvashaLauncherModrinthProject *project;
} ModpackPickerList;

static void
modpack_picker_list_free (ModpackPickerList *list)
{
  if (list == NULL)
    return;
  g_clear_pointer (&list->versions, g_ptr_array_unref);
  g_clear_pointer (&list->project, nuvashalauncher_modrinth_project_free);
  g_free (list);
}

static GHashTable *
get_active_modpack_downloads (void)
{
  static GHashTable *active = NULL;
  if (active == NULL)
    active = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return active;
}

typedef struct {
  char *version_id;
  GWeakRef button_ref;
  GWeakRef spinner_ref;
} ModpackPickerDownloadJob;

static void
modpack_picker_download_job_free (gpointer data)
{
  ModpackPickerDownloadJob *job = data;
  if (job == NULL)
    return;
  g_free (job->version_id);
  g_weak_ref_clear (&job->button_ref);
  g_weak_ref_clear (&job->spinner_ref);
  g_free (job);
}

static void
on_modpack_picker_download_finished (gboolean success, gpointer user_data)
{
  ModpackPickerDownloadJob *job = user_data;
  GtkWidget *button = g_weak_ref_get (&job->button_ref);
  GtkWidget *spinner = g_weak_ref_get (&job->spinner_ref);

  if (job->version_id != NULL)
    g_hash_table_remove (get_active_modpack_downloads (), job->version_id);

  if (spinner != NULL)
    {
      gtk_widget_set_visible (spinner, FALSE);
      g_object_unref (spinner);
    }

  if (button != NULL)
    {
      if (success)
        {
          gtk_button_set_label (GTK_BUTTON (button), _("Installed"));
          gtk_widget_set_sensitive (button, FALSE);
        }
      else
        {
          gtk_button_set_label (GTK_BUTTON (button), _("Download"));
          gtk_widget_set_sensitive (button, TRUE);
        }
      g_object_unref (button);
    }
}

typedef struct {
  NuvashaLauncherDownloadModpackDialog *dialog;
  NuvashaLauncherModrinthProject *project;
  NuvashaLauncherModrinthVersion *version;
  GtkButton *button;
  GtkWidget *spinner;
} ModpackPickerRowAction;

static void
modpack_picker_row_action_free (gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  ModpackPickerRowAction *action = data;
  if (action == NULL)
    return;
  g_clear_pointer (&action->project, nuvashalauncher_modrinth_project_free);
  g_clear_pointer (&action->version, nuvashalauncher_modrinth_version_free);
  g_free (action);
}

static void
on_modpack_picker_download_clicked (GtkButton *button, gpointer user_data)
{
  ModpackPickerRowAction *action = user_data;
  NuvashaLauncherDownloadModpackDialog *self = action->dialog;
  const char *vid;
  ModpackPickerDownloadJob *job;

  if (self == NULL || action->project == NULL || action->version == NULL)
    return;

  vid = action->version->version_id;
  if (vid != NULL && g_hash_table_contains (get_active_modpack_downloads (), vid))
    return;

  if (vid != NULL)
    g_hash_table_add (get_active_modpack_downloads (), g_strdup (vid));

  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_button_set_label (button, _("Downloading…"));
  gtk_widget_set_visible (action->spinner, TRUE);

  job = g_new0 (ModpackPickerDownloadJob, 1);
  job->version_id = g_strdup (vid);
  g_weak_ref_init (&job->button_ref, button);
  g_weak_ref_init (&job->spinner_ref, action->spinner);

  start_modpack_direct_install (self, action->project, action->version,
                                on_modpack_picker_download_finished,
                                job, modpack_picker_download_job_free);
}

static void
append_modpack_version_rows (ModpackPickerList *list,
                             NuvashaLauncherDownloadModpackDialog *self,
                             guint batch_size)
{
  guint end = MIN (list->loaded + batch_size, list->versions->len);

  for (; list->loaded < end; list->loaded++)
    {
      NuvashaLauncherModrinthVersion *v = g_ptr_array_index (list->versions, list->loaded);
      char label[160];
      AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
      GtkWidget *spinner = GTK_WIDGET (adw_spinner_new ());
      GtkButton *button = GTK_BUTTON (gtk_button_new ());
      gboolean downloading = v->version_id != NULL &&
                             g_hash_table_contains (get_active_modpack_downloads (),
                                                    v->version_id);
      ModpackPickerRowAction *action;

      version_row_label (v, label, sizeof label);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), label);
      if (v->name != NULL && *v->name != '\0' &&
          v->version_number != NULL && g_strcmp0 (v->name, v->version_number) != 0)
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), v->name);

      gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
      gtk_widget_set_visible (spinner, downloading);
      adw_action_row_add_suffix (row, spinner);

      gtk_widget_set_valign (GTK_WIDGET (button), GTK_ALIGN_CENTER);
      if (downloading)
        {
          gtk_button_set_label (button, _("Downloading…"));
          gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
        }
      else
        {
          gtk_button_set_label (button, _("Download"));
        }

      action = g_new0 (ModpackPickerRowAction, 1);
      action->dialog = self;
      action->project = nuvashalauncher_modrinth_project_copy (list->project);
      action->version = nuvashalauncher_modrinth_version_copy (v);
      action->button = button;
      action->spinner = spinner;
      g_signal_connect_data (button, "clicked",
                             G_CALLBACK (on_modpack_picker_download_clicked),
                             action, modpack_picker_row_action_free, 0);

      adw_action_row_add_suffix (row, GTK_WIDGET (button));
      adw_preferences_group_add (list->group, GTK_WIDGET (row));
    }
}

static void
on_modpack_picker_scroll_changed (GtkAdjustment *adjustment, gpointer user_data)
{
  NuvashaLauncherDownloadModpackDialog *self =
    NUVASHALAUNCHER_DOWNLOAD_MODPACK_DIALOG (user_data);
  ModpackPickerList *list =
    g_object_get_data (G_OBJECT (adjustment), "nuvashalauncher-version-list");
  double value, page_size, upper;

  if (list == NULL || list->versions == NULL || list->loaded >= list->versions->len)
    return;

  value = gtk_adjustment_get_value (adjustment);
  page_size = gtk_adjustment_get_page_size (adjustment);
  upper = gtk_adjustment_get_upper (adjustment);

  if (value + page_size >= upper - 120.0)
    append_modpack_version_rows (list, self, VERSION_PICKER_BATCH_SIZE);
}

static void
on_modpack_picker_closed (AdwDialog *dialog, gpointer user_data)
{
  NuvashaLauncherDownloadModpackDialog *self =
    NUVASHALAUNCHER_DOWNLOAD_MODPACK_DIALOG (user_data);
  GCancellable *cancellable =
    g_object_get_data (G_OBJECT (dialog), "nuvashalauncher-cancellable");

  if (cancellable != NULL)
    g_cancellable_cancel (cancellable);

  if (g_object_get_data (G_OBJECT (self), "nuvashalauncher-version-picker") == dialog)
    g_object_set_data (G_OBJECT (self), "nuvashalauncher-version-picker", NULL);
}

static void
on_modpack_choose_versions_ready (GObject *source G_GNUC_UNUSED,
                                  GAsyncResult *result,
                                  gpointer user_data)
{
  ModpackVersionFetch *fetch = user_data;
  NuvashaLauncherDownloadModpackDialog *self = fetch->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser =
    nuvashalauncher_download_json_finish (result, &error);

  if (fetch->picker == NULL || g_cancellable_is_cancelled (fetch->cancellable) ||
      self == NULL || self->closed)
    {
      modpack_version_fetch_free (fetch);
      return;
    }

  if (error != NULL)
    {
      adw_dialog_close (fetch->picker);
      if (!nuvashalauncher_error_is_cancelled (error))
        push_failure (self, _("Could not load versions. Try again."), error, NULL);
      modpack_version_fetch_free (fetch);
      return;
    }

  g_autoptr (GPtrArray) versions =
    versions_of_answer (json_parser_get_root (parser),
                        self->filter_loaders, self->filter_versions, &error);
  if (versions == NULL)
    {
      adw_dialog_close (fetch->picker);
      push_failure (self, _("Could not load versions. Try again."), error, NULL);
      modpack_version_fetch_free (fetch);
      return;
    }

  if (versions->len == 0)
    {
      AdwStatusPage *status = ADW_STATUS_PAGE (adw_status_page_new ());
      adw_status_page_set_icon_name (status, "package-x-generic-symbolic");
      adw_status_page_set_title (
        status,
        _("The requested file, project, or compatible version was not found."));
      AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
      adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (adw_header_bar_new ()));
      adw_toolbar_view_set_content (toolbar, GTK_WIDGET (status));
      adw_dialog_set_child (fetch->picker, GTK_WIDGET (toolbar));
      modpack_version_fetch_free (fetch);
      return;
    }

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  ModpackPickerList *list = g_new0 (ModpackPickerList, 1);
  list->versions = g_ptr_array_ref (versions);
  list->loaded = 0;
  list->group = group;
  list->picker = fetch->picker;
  list->project = nuvashalauncher_modrinth_project_copy (fetch->project);

  g_object_set_data_full (G_OBJECT (fetch->picker), "nuvashalauncher-version-list",
                          list, (GDestroyNotify) modpack_picker_list_free);

  append_modpack_version_rows (list, self, VERSION_PICKER_BATCH_SIZE);

  AdwClamp *clamp = ADW_CLAMP (adw_clamp_new ());
  gtk_widget_set_margin_top (GTK_WIDGET (clamp), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (clamp), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (clamp), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (clamp), 12);
  adw_clamp_set_child (clamp, GTK_WIDGET (group));

  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (scroller, GTK_WIDGET (clamp));
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);

  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (scroller);
  g_object_set_data (G_OBJECT (adjustment), "nuvashalauncher-version-list", list);
  g_signal_connect_object (adjustment, "value-changed",
                           G_CALLBACK (on_modpack_picker_scroll_changed), self, 0);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (adw_header_bar_new ()));
  adw_toolbar_view_set_content (toolbar, GTK_WIDGET (scroller));
  adw_dialog_set_child (fetch->picker, GTK_WIDGET (toolbar));
  modpack_version_fetch_free (fetch);
}

void
modpack_choose_version (NuvashaLauncherDownloadModpackDialog *self,
                        NuvashaLauncherModrinthProject *project)
{
  if (self == NULL || project == NULL || project->project_id == NULL)
    return;

  if (g_object_get_data (G_OBJECT (self), "nuvashalauncher-version-picker") != NULL)
    return;

  AdwDialog *picker = adw_dialog_new ();
  adw_dialog_set_title (picker, project->title != NULL ? project->title : _("Versions"));
  adw_dialog_set_content_width (picker, 420);
  adw_dialog_set_content_height (picker, 440);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (adw_header_bar_new ()));
  adw_toolbar_view_set_content (toolbar, nuvashalauncher_ui_loading_child ());
  adw_dialog_set_child (picker, GTK_WIDGET (toolbar));

  ModpackVersionFetch *fetch = g_new0 (ModpackVersionFetch, 1);
  fetch->dialog = g_object_ref (self);
  fetch->project = nuvashalauncher_modrinth_project_copy (project);
  fetch->picker = g_object_ref (picker);
  fetch->cancellable = g_cancellable_new ();

  g_object_set_data (G_OBJECT (self), "nuvashalauncher-version-picker", picker);
  g_object_set_data_full (G_OBJECT (picker), "nuvashalauncher-cancellable",
                          g_object_ref (fetch->cancellable), g_object_unref);
  g_signal_connect_object (picker, "closed",
                           G_CALLBACK (on_modpack_picker_closed),
                           self, 0);

  adw_dialog_present (picker, GTK_WIDGET (self));

  g_autofree char *escaped_id = g_uri_escape_string (project->project_id, NULL, FALSE);
  g_autofree char *url =
    g_strdup_printf (NUVASHALAUNCHER_MODRINTH_URL "/project/%s/version", escaped_id);

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_json_cached_async (
      nuvashalauncher_downloads_get_service (), url,
      on_modpack_choose_versions_ready, fetch),
    fetch->cancellable);
}

typedef struct {
  NuvashaLauncherDownloadModpackDialog *dialog;
  NuvashaLauncherModrinthProject *project;
} BatchModpackVersionFetch;

static void
on_batch_single_modpack_versions_ready (GObject *source G_GNUC_UNUSED,
                                        GAsyncResult *result,
                                        gpointer user_data)
{
  BatchModpackVersionFetch *fetch = user_data;
  NuvashaLauncherDownloadModpackDialog *runner = fetch->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser =
    nuvashalauncher_download_json_finish (result, &error);

  if (parser != NULL && error == NULL)
    {
      g_autoptr (GPtrArray) versions =
        versions_of_answer (json_parser_get_root (parser),
                            runner->filter_loaders, runner->filter_versions,
                            NULL);
      if (versions != NULL && versions->len > 0)
        {
          NuvashaLauncherModrinthVersion *latest = g_ptr_array_index (versions, 0);
          do_start_modpack_install (runner, fetch->project, latest);
        }
    }

  g_clear_object (&fetch->dialog);
  g_clear_pointer (&fetch->project, nuvashalauncher_modrinth_project_free);
  g_free (fetch);
}

typedef struct {
  NuvashaLauncherDownloadModpackDialog *dialog;
  GPtrArray *projects;
} BatchModpackConfirm;

static void
batch_modpack_confirm_free (BatchModpackConfirm *confirm)
{
  if (confirm == NULL)
    return;
  g_clear_object (&confirm->dialog);
  g_clear_pointer (&confirm->projects, g_ptr_array_unref);
  g_free (confirm);
}

static void
on_modpack_batch_confirmed (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncherDownloadModpackDialog *self =
    NUVASHALAUNCHER_DOWNLOAD_MODPACK_DIALOG (user_data);
  BatchModpackConfirm *confirm =
    g_object_get_data (G_OBJECT (source), "nuvashalauncher-modpack-batch-confirm");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (confirm == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (g_strcmp0 (response, "install") == 0 && !self->closed)
    {
      modpack_dialog_clear_selection (self);
      for (guint i = 0; i < confirm->projects->len; i++)
        {
          NuvashaLauncherModrinthProject *p = g_ptr_array_index (confirm->projects, i);
          NuvashaLauncherDownloadModpackDialog *runner =
            g_object_ref_sink (g_object_new (NUVASHALAUNCHER_TYPE_DOWNLOAD_MODPACK_DIALOG, NULL));
          runner->manager = g_object_ref (self->manager);
          runner->closed = TRUE;
          g_clear_pointer (&runner->filter_loaders, g_ptr_array_unref);
          g_clear_pointer (&runner->filter_versions, g_ptr_array_unref);
          if (self->filter_loaders != NULL)
            runner->filter_loaders = g_ptr_array_ref (self->filter_loaders);
          if (self->filter_versions != NULL)
            runner->filter_versions = g_ptr_array_ref (self->filter_versions);

          BatchModpackVersionFetch *fetch = g_new0 (BatchModpackVersionFetch, 1);
          fetch->dialog = runner;
          fetch->project = nuvashalauncher_modrinth_project_copy (p);

          g_autofree char *escaped_id = g_uri_escape_string (p->project_id, NULL, FALSE);
          g_autofree char *url =
            g_strdup_printf (NUVASHALAUNCHER_MODRINTH_URL "/project/%s/version", escaped_id);

          nuvashalauncher_task_scope_release_when_done (
            nuvashalauncher_download_json_cached_async (
              nuvashalauncher_downloads_get_service (), url,
              on_batch_single_modpack_versions_ready, fetch),
            NULL);
        }
    }

  batch_modpack_confirm_free (confirm);
  g_object_unref (self);
}

void
modpack_dialog_install_batch (NuvashaLauncherDownloadModpackDialog *self,
                              GPtrArray *projects)
{
  if (self == NULL || self->closed || projects == NULL || projects->len == 0)
    {
      if (projects != NULL)
        g_ptr_array_unref (projects);
      return;
    }

  guint count = projects->len;
  const char *heading_format =
    ngettext ("Install the following modpack?", "Install the following modpacks?", count);
  g_autofree char *heading = g_strdup (heading_format);

  g_autoptr (GString) body = g_string_new (NULL);
  for (guint i = 0; i < projects->len; i++)
    {
      NuvashaLauncherModrinthProject *p = g_ptr_array_index (projects, i);
      g_string_append_printf (body, "%s\n", p->title != NULL ? p->title : p->project_id);
    }
  while (body->len > 0 && body->str[body->len - 1] == '\n')
    g_string_truncate (body, body->len - 1);

  AdwAlertDialog *alert =
    ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body->str));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "install", _("Install"));
  adw_alert_dialog_set_response_appearance (alert, "install",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  BatchModpackConfirm *confirm = g_new0 (BatchModpackConfirm, 1);
  confirm->dialog = g_object_ref (self);
  confirm->projects = projects;

  g_object_set_data (G_OBJECT (alert), "nuvashalauncher-modpack-batch-confirm", confirm);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_modpack_batch_confirmed, g_object_ref (self));
}
