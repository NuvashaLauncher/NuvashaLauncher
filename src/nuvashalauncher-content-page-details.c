/* Copyright (C) 2026 Flia
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
} VersionPickerList;

static void
version_picker_list_free (VersionPickerList *list)
{
  if (list == NULL)
    return;
  g_clear_pointer (&list->versions, g_ptr_array_unref);
  g_free (list->project_title);
  g_free (list);
}

static void
on_picker_version_activated (AdwActionRow *row, gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  NuvashaLauncherModrinthVersion *version =
    g_object_get_data (G_OBJECT (row), "nuvashalauncher-version");
  const char *project_title =
    g_object_get_data (G_OBJECT (row), "nuvashalauncher-project-title");
  AdwDialog *dialog = g_object_get_data (G_OBJECT (row), "nuvashalauncher-picker-dialog");

  if (self == NULL || version == NULL)
    return;

  NuvashaLauncherModrinthVersion *copy = nuvashalauncher_modrinth_version_copy (version);
  g_autofree char *title = g_strdup (project_title != NULL ? project_title
                                                           : version_name_of (version));

  if (dialog != NULL)
    adw_dialog_close (dialog);

  start_plan (self, copy, title, NULL, FALSE);
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
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), label);
      if (v->version_number != NULL && *v->version_number != '\0' &&
          g_strcmp0 (label, v->version_number) != 0)
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), v->version_number);

      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

      g_object_set_data_full (G_OBJECT (row), "nuvashalauncher-version",
                              nuvashalauncher_modrinth_version_copy (v),
                              (GDestroyNotify) nuvashalauncher_modrinth_version_free);
      g_object_set_data_full (G_OBJECT (row), "nuvashalauncher-project-title",
                              g_strdup (list->project_title), g_free);
      g_object_set_data (G_OBJECT (row), "nuvashalauncher-picker-dialog",
                         list->dialog);
      g_signal_connect_object (row, "activated",
                               G_CALLBACK (on_picker_version_activated), self, 0);
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
  VersionPickerList *list = g_new0 (VersionPickerList, 1);
  list->versions = g_ptr_array_ref (versions);
  list->loaded = 0;
  list->group = group;
  list->dialog = fetch->dialog;
  list->project_title = g_strdup (fetch->project_title);

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
  adw_dialog_set_content_width (dialog, 420);
  adw_dialog_set_content_height (dialog, 440);

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
