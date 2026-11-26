/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-task.h"
#include "nuvashalauncher-ui-feedback.h"

#define VERSION_PICKER_BATCH_SIZE 25

/* Return the best available display name for a version. */
const char *
version_name_of (NuvashaLauncherModrinthVersion *version)
{
  if (version->name != NULL && *version->name != '\0')
    return version->name;

  if (version->version_number != NULL && *version->version_number != '\0')
    return version->version_number;

  return version->version_id != NULL ? version->version_id : "the file";
}

/* Format a version label with its publication date. */
static char *
version_label (NuvashaLauncherModrinthVersion *version)
{
  if (version->date_published != NULL && *version->date_published != '\0')
    {
      g_autofree char *when = g_strndup (version->date_published, 10);

      return g_strdup_printf ("%s  ·  %s", version_name_of (version), when);
    }

  return g_strdup (version_name_of (version));
}

typedef struct {
  NuvashaLauncherContentPage *page;
  char *project_title;
  AdwDialog *dialog;
  NuvashaLauncherTaskScope *operation;
  GCancellable *cancellable;
} RowVersionFetch;

static void
row_version_fetch_free (RowVersionFetch *fetch)
{
  if (fetch == NULL)
    return;
  if (fetch->operation != NULL)
    nuvashalauncher_task_scope_unref (fetch->operation);
  g_clear_object (&fetch->cancellable);
  g_clear_object (&fetch->dialog);
  g_free (fetch->project_title);
  g_free (fetch);
}

static void
on_quick_install_versions_ready (GObject *source G_GNUC_UNUSED,
                                 GAsyncResult *result,
                                 gpointer user_data)
{
  RowVersionFetch *fetch = user_data;
  NuvashaLauncherContentPage *self = fetch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions =
    nuvashalauncher_modrinth_versions_finish (result, &error);

  if (self == NULL || self->browse.model == NULL ||
      g_cancellable_is_cancelled (self->browse.model->cancellable))
    {
      row_version_fetch_free (fetch);
      return;
    }

  if (error != NULL)
    {
      if (!nuvashalauncher_error_is_cancelled (error))
        show_error (self, _("Could not load versions. Try again."), error);
      row_version_fetch_free (fetch);
      return;
    }

  if (versions == NULL || versions->len == 0)
    {
      g_autofree char *msg = g_strdup_printf (
        _("No compatible version found for Minecraft %s."), self->minecraft_version);
      show_toast (self, msg);
      row_version_fetch_free (fetch);
      return;
    }

  NuvashaLauncherModrinthVersion *latest = g_ptr_array_index (versions, 0);
  start_plan (self, nuvashalauncher_modrinth_version_copy (latest),
              fetch->project_title != NULL ? fetch->project_title
                                           : version_name_of (latest),
              NULL, FALSE);
  row_version_fetch_free (fetch);
}

void
nuvashalauncher_content_page_quick_install_project (NuvashaLauncherContentPage *self,
                                                    NuvashaLauncherModrinthProject *project)
{
  if (self == NULL || project == NULL || project->project_id == NULL)
    return;

  RowVersionFetch *fetch = g_new0 (RowVersionFetch, 1);
  fetch->page = self;
  fetch->project_title = g_strdup (project->title);
  fetch->operation = nuvashalauncher_modrinth_versions_async (
    project->project_id, self->type, self->minecraft_version, self->loader,
    on_quick_install_versions_ready, fetch);
  nuvashalauncher_task_scope_follow_cancellable (fetch->operation,
                                     self->browse.model->cancellable);
}

typedef struct {
  GPtrArray *versions;
  guint loaded;
  AdwPreferencesGroup *group;
  AdwDialog *dialog;
  char *project_title;
  char *installed_version_id;
  GPtrArray *row_buttons; /* GtkButton* borrowed while dialog lives */
} VersionPickerList;

static GHashTable *active_version_downloads = NULL;

static GHashTable *
get_active_version_downloads (void)
{
  if (active_version_downloads == NULL)
    active_version_downloads = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return active_version_downloads;
}

static void
version_picker_list_free (VersionPickerList *list)
{
  if (list == NULL)
    return;
  g_clear_pointer (&list->versions, g_ptr_array_unref);
  g_clear_pointer (&list->row_buttons, g_ptr_array_unref);
  g_free (list->project_title);
  g_free (list->installed_version_id);
  g_free (list);
}

typedef struct {
  char *version_id;
  GWeakRef dialog_ref;
  GWeakRef button_ref;
  GWeakRef spinner_ref;
} PickerDownloadJob;

static void
picker_download_job_free (gpointer data)
{
  PickerDownloadJob *job = data;

  if (job == NULL)
    return;

  if (job->version_id != NULL && active_version_downloads != NULL)
    g_hash_table_remove (active_version_downloads, job->version_id);

  g_free (job->version_id);
  g_weak_ref_clear (&job->dialog_ref);
  g_weak_ref_clear (&job->button_ref);
  g_weak_ref_clear (&job->spinner_ref);
  g_free (job);
}

static void
on_picker_download_finished (gboolean success, gpointer user_data)
{
  PickerDownloadJob *job = user_data;
  g_autoptr (AdwDialog) dialog = g_weak_ref_get (&job->dialog_ref);
  g_autoptr (GtkButton) button = g_weak_ref_get (&job->button_ref);
  g_autoptr (GtkWidget) spinner = g_weak_ref_get (&job->spinner_ref);

  if (job->version_id != NULL && active_version_downloads != NULL)
    g_hash_table_remove (active_version_downloads, job->version_id);

  if (spinner != NULL)
    gtk_widget_set_visible (spinner, FALSE);

  if (success)
    {
      if (dialog != NULL)
        {
          VersionPickerList *list =
            g_object_get_data (G_OBJECT (dialog), "nuvashalauncher-version-list");
          if (list != NULL)
            {
              g_free (list->installed_version_id);
              list->installed_version_id = g_strdup (job->version_id);

              for (guint i = 0; list->row_buttons != NULL && i < list->row_buttons->len; i++)
                {
                  GtkButton *other = g_ptr_array_index (list->row_buttons, i);
                  const char *other_vid =
                    g_object_get_data (G_OBJECT (other), "nuvashalauncher-version-id");
                  if (other == button)
                    continue;
                  if (other_vid != NULL &&
                      g_hash_table_contains (get_active_version_downloads (), other_vid))
                    continue;
                  gtk_button_set_label (other, _("Download"));
                  gtk_widget_set_sensitive (GTK_WIDGET (other), TRUE);
                }
            }
        }

      if (button != NULL)
        {
          gtk_button_set_label (button, _("Installed"));
          gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
        }
    }
  else
    {
      if (button != NULL)
        {
          gtk_button_set_label (button, _("Download"));
          gtk_widget_set_sensitive (GTK_WIDGET (button), TRUE);
        }
    }
}

typedef struct {
  NuvashaLauncherContentPage *page; /* borrowed; connected with connect_object on page */
  NuvashaLauncherModrinthVersion *version;
  char *project_title;
  AdwDialog *dialog;
  GtkButton *button;
  GtkWidget *spinner;
} PickerRowAction;

static void
picker_row_action_free (gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  PickerRowAction *action = data;

  if (action == NULL)
    return;
  g_clear_pointer (&action->version, nuvashalauncher_modrinth_version_free);
  g_free (action->project_title);
  g_free (action);
}

static void
on_picker_download_clicked (GtkButton *button, gpointer user_data)
{
  PickerRowAction *action = user_data;
  NuvashaLauncherContentPage *self = action->page;
  const char *vid;
  PickerDownloadJob *job;

  if (self == NULL || action->version == NULL)
    return;

  vid = action->version->version_id;
  if (vid != NULL && g_hash_table_contains (get_active_version_downloads (), vid))
    return;

  if (vid != NULL)
    g_hash_table_add (get_active_version_downloads (), g_strdup (vid));

  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_button_set_label (button, _("Downloading…"));
  gtk_widget_set_visible (action->spinner, TRUE);

  job = g_new0 (PickerDownloadJob, 1);
  job->version_id = g_strdup (vid);
  g_weak_ref_init (&job->dialog_ref, action->dialog);
  g_weak_ref_init (&job->button_ref, button);
  g_weak_ref_init (&job->spinner_ref, action->spinner);

  start_plan_direct (self,
                     nuvashalauncher_modrinth_version_copy (action->version),
                     action->project_title != NULL ? action->project_title
                                                   : version_name_of (action->version),
                     on_picker_download_finished,
                     job,
                     picker_download_job_free);
}

static void
append_version_rows (VersionPickerList *list,
                     NuvashaLauncherContentPage *self,
                     guint batch_size)
{
  guint end = MIN (list->loaded + batch_size, list->versions->len);

  for (; list->loaded < end; list->loaded++)
    {
      NuvashaLauncherModrinthVersion *v = g_ptr_array_index (list->versions, list->loaded);
      g_autofree char *label = version_label (v);
      AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
      GtkWidget *spinner = GTK_WIDGET (adw_spinner_new ());
      GtkButton *button = GTK_BUTTON (gtk_button_new ());
      gboolean downloading = v->version_id != NULL &&
                             g_hash_table_contains (get_active_version_downloads (),
                                                    v->version_id);
      gboolean installed = list->installed_version_id != NULL &&
                           g_strcmp0 (list->installed_version_id, v->version_id) == 0;
      PickerRowAction *action;

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), label);
      if (v->version_number != NULL && *v->version_number != '\0' &&
          g_strcmp0 (label, v->version_number) != 0)
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), v->version_number);

      gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
      gtk_widget_set_visible (spinner, downloading);
      adw_action_row_add_suffix (row, spinner);

      gtk_widget_set_valign (GTK_WIDGET (button), GTK_ALIGN_CENTER);
      if (downloading)
        {
          gtk_button_set_label (button, _("Downloading…"));
          gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
        }
      else if (installed)
        {
          gtk_button_set_label (button, _("Installed"));
          gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
        }
      else
        {
          gtk_button_set_label (button, _("Download"));
        }

      g_object_set_data_full (G_OBJECT (button), "nuvashalauncher-version-id",
                              g_strdup (v->version_id), g_free);

      action = g_new0 (PickerRowAction, 1);
      action->page = self;
      action->version = nuvashalauncher_modrinth_version_copy (v);
      action->project_title = g_strdup (list->project_title);
      action->dialog = list->dialog;
      action->button = button;
      action->spinner = spinner;
      g_signal_connect_data (button, "clicked",
                             G_CALLBACK (on_picker_download_clicked),
                             action, picker_row_action_free, 0);

      adw_action_row_add_suffix (row, GTK_WIDGET (button));
      g_ptr_array_add (list->row_buttons, button);
      adw_preferences_group_add (list->group, GTK_WIDGET (row));
    }
}

static void
on_picker_scroll_changed (GtkAdjustment *adjustment, gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  VersionPickerList *list =
    g_object_get_data (G_OBJECT (adjustment), "nuvashalauncher-version-list");
  double value, page_size, upper;

  if (list == NULL || list->versions == NULL || list->loaded >= list->versions->len)
    return;

  value = gtk_adjustment_get_value (adjustment);
  page_size = gtk_adjustment_get_page_size (adjustment);
  upper = gtk_adjustment_get_upper (adjustment);

  if (value + page_size >= upper - 120.0)
    append_version_rows (list, self, VERSION_PICKER_BATCH_SIZE);
}

static void
on_picker_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  GCancellable *cancellable =
    g_object_get_data (G_OBJECT (dialog), "nuvashalauncher-cancellable");

  if (cancellable != NULL)
    g_cancellable_cancel (cancellable);

  if (g_object_get_data (G_OBJECT (self), "nuvashalauncher-version-picker") == dialog)
    g_object_set_data (G_OBJECT (self), "nuvashalauncher-version-picker", NULL);
}

static void
on_choose_versions_ready (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result,
                          gpointer user_data)
{
  RowVersionFetch *fetch = user_data;
  NuvashaLauncherContentPage *self = fetch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions =
    nuvashalauncher_modrinth_versions_finish (result, &error);

  if (fetch->dialog == NULL || g_cancellable_is_cancelled (fetch->cancellable) ||
      self == NULL || self->browse.model == NULL ||
      g_cancellable_is_cancelled (self->browse.model->cancellable))
    {
      row_version_fetch_free (fetch);
      return;
    }

  if (error != NULL)
    {
      adw_dialog_close (fetch->dialog);
      if (!nuvashalauncher_error_is_cancelled (error))
        show_error (self, _("Could not load versions. Try again."), error);
      row_version_fetch_free (fetch);
      return;
    }

  if (versions == NULL || versions->len == 0)
    {
      g_autofree char *msg = g_strdup_printf (
        _("No compatible version found for Minecraft %s."), self->minecraft_version);
      AdwStatusPage *status = ADW_STATUS_PAGE (adw_status_page_new ());
      adw_status_page_set_icon_name (status, nuvashalauncher_content_type_icon (self->type));
      adw_status_page_set_title (status, msg);
      AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
      adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (adw_header_bar_new ()));
      adw_toolbar_view_set_content (toolbar, GTK_WIDGET (status));
      adw_dialog_set_child (fetch->dialog, GTK_WIDGET (toolbar));
      row_version_fetch_free (fetch);
      return;
    }

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  gtk_widget_add_css_class (GTK_WIDGET (group), "nuvashalauncher-preferences-surface");
  VersionPickerList *list = g_new0 (VersionPickerList, 1);
  list->versions = g_ptr_array_ref (versions);
  list->loaded = 0;
  list->group = group;
  list->dialog = fetch->dialog;
  list->project_title = g_strdup (fetch->project_title);
  list->row_buttons = g_ptr_array_new ();

  if (versions->len > 0)
    {
      NuvashaLauncherModrinthVersion *first = g_ptr_array_index (versions, 0);
      NuvashaLauncherContentFile *installed =
        installed_project_file (self, first->project_id);
      if (installed != NULL && installed->version_id != NULL)
        list->installed_version_id = g_strdup (installed->version_id);
    }

  g_object_set_data_full (G_OBJECT (fetch->dialog), "nuvashalauncher-version-list",
                          list, (GDestroyNotify) version_picker_list_free);

  append_version_rows (list, self, VERSION_PICKER_BATCH_SIZE);

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
                           G_CALLBACK (on_picker_scroll_changed), self, 0);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (adw_header_bar_new ()));
  adw_toolbar_view_set_content (toolbar, GTK_WIDGET (scroller));
  adw_dialog_set_child (fetch->dialog, GTK_WIDGET (toolbar));
  row_version_fetch_free (fetch);
}

void
nuvashalauncher_content_page_choose_version (NuvashaLauncherContentPage *self,
                                           NuvashaLauncherModrinthProject *project)
{
  if (self == NULL || project == NULL || project->project_id == NULL)
    return;

  if (g_object_get_data (G_OBJECT (self), "nuvashalauncher-version-picker") != NULL)
    return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, project->title != NULL ? project->title : _("Versions"));
  adw_dialog_set_content_width (dialog, 520);
  adw_dialog_set_content_height (dialog, 460);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (adw_header_bar_new ()));
  adw_toolbar_view_set_content (toolbar, nuvashalauncher_ui_loading_child ());
  adw_dialog_set_child (dialog, GTK_WIDGET (toolbar));

  RowVersionFetch *fetch = g_new0 (RowVersionFetch, 1);
  fetch->page = self;
  fetch->project_title = g_strdup (project->title);
  fetch->dialog = g_object_ref (dialog);
  fetch->cancellable = g_cancellable_new ();

  g_object_set_data (G_OBJECT (self), "nuvashalauncher-version-picker", dialog);
  g_object_set_data_full (G_OBJECT (dialog), "nuvashalauncher-cancellable",
                          g_object_ref (fetch->cancellable), g_object_unref);
  g_signal_connect_object (dialog, "closed",
                           G_CALLBACK (on_picker_dialog_closed),
                           self, 0);

  adw_dialog_present (dialog, GTK_WIDGET (self));

  fetch->operation = nuvashalauncher_modrinth_versions_async (
    project->project_id, self->type, self->minecraft_version, self->loader,
    on_choose_versions_ready, fetch);
  nuvashalauncher_task_scope_follow_cancellable (fetch->operation, fetch->cancellable);
}
