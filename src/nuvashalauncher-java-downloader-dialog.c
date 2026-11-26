/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Dialog for browsing and downloading Eclipse Temurin JDKs for the host OS. */
#include "nuvashalauncher-java-downloader-dialog.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-errors.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-java-download.h"

#define JAVA_DOWNLOADER_DIALOG_RESOURCE \
  "/com/github/NuvashaLauncher/NuvashaLauncher/ui/nuvashalauncher-java-downloader-dialog.ui"

struct _NuvashaLauncherJavaDownloaderDialog
{
  AdwDialog parent_instance;

  AdwToastOverlay *toast_overlay;
  GtkStack *stack;
  AdwStatusPage *error_page;
  GtkButton *retry_button;
  AdwPreferencesGroup *releases_group;

  GWeakRef java_row_ref;
  NuvashaLauncherTaskScope *list_scope;
  GPtrArray *rows; /* GtkWidget* added to releases_group */
};

G_DEFINE_FINAL_TYPE (NuvashaLauncherJavaDownloaderDialog,
                     nuvashalauncher_java_downloader_dialog,
                     ADW_TYPE_DIALOG)

/* Process-wide set of release install directory names currently downloading,
 * so reopening the dialog reflects active downloads and avoids duplicates. */
static GHashTable *active_installs = NULL;

static GHashTable *
get_active_installs (void)
{
  if (active_installs == NULL)
    active_installs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return active_installs;
}

static void
show_toast (NuvashaLauncherJavaDownloaderDialog *self, const char *message)
{
  if (self == NULL || self->toast_overlay == NULL || message == NULL)
    return;

  AdwToast *toast = adw_toast_new (message);
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

typedef struct {
  NuvashaLauncherJavaRelease *release;
  char *dir_name;
  NuvashaLauncherDownload *download;
  GWeakRef dialog_ref;
  GWeakRef button_ref;
  GWeakRef spinner_ref;
  GWeakRef java_row_ref;
} InstallJob;

static void
install_job_free (InstallJob *job)
{
  if (job == NULL)
    return;

  if (job->dir_name != NULL && active_installs != NULL)
    g_hash_table_remove (active_installs, job->dir_name);

  g_clear_pointer (&job->release, nuvashalauncher_java_release_free);
  g_free (job->dir_name);
  g_clear_object (&job->download);
  g_weak_ref_clear (&job->dialog_ref);
  g_weak_ref_clear (&job->button_ref);
  g_weak_ref_clear (&job->spinner_ref);
  g_weak_ref_clear (&job->java_row_ref);
  g_free (job);
}

static void
on_install_progress (NuvashaLauncherJavaDownloadStage stage,
                     goffset bytes_done,
                     goffset bytes_total,
                     gpointer user_data)
{
  InstallJob *job = user_data;

  if (job->download == NULL)
    return;

  if (stage == NUVASHALAUNCHER_JAVA_DOWNLOAD_STAGE_DOWNLOADING)
    {
      nuvashalauncher_download_set_detail (job->download, _("Downloading…"));
      nuvashalauncher_download_set_bytes (
        job->download,
        bytes_done > 0 ? (guint64) bytes_done : 0,
        bytes_total > 0 ? (guint64) bytes_total : 0);
    }
  else
    {
      nuvashalauncher_download_set_detail (job->download, _("Extracting…"));
      nuvashalauncher_download_set_progress (job->download, 0, 0);
    }
}

static void
on_install_done (GObject *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer user_data)
{
  InstallJob *job = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *java_exe = nuvashalauncher_java_download_install_finish (result, &error);
  g_autoptr (NuvashaLauncherJavaDownloaderDialog) dialog = g_weak_ref_get (&job->dialog_ref);
  g_autoptr (GtkButton) button = g_weak_ref_get (&job->button_ref);
  g_autoptr (GtkWidget) spinner = g_weak_ref_get (&job->spinner_ref);
  g_autoptr (NuvashaLauncherJavaRow) java_row = g_weak_ref_get (&job->java_row_ref);

  if (job->download != NULL)
    nuvashalauncher_download_finish (job->download);

  if (spinner != NULL)
    gtk_widget_set_visible (spinner, FALSE);

  if (java_exe != NULL)
    {
      if (button != NULL)
        {
          gtk_button_set_label (button, _("Installed"));
          gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
        }

      if (java_row != NULL)
        {
          const char *selected = nuvashalauncher_java_row_get_selected_path (java_row);
          g_autofree char *wanted = (selected != NULL && *selected != '\0')
                                      ? g_strdup (selected)
                                      : NULL;
          nuvashalauncher_java_row_load (java_row, wanted);
        }

      if (dialog != NULL)
        {
          g_autofree char *msg = g_strdup_printf (
            _("Installed Eclipse Temurin %d."), job->release->major_version);
          show_toast (dialog, msg);
        }
    }
  else
    {
      if (button != NULL)
        {
          gtk_button_set_label (button, _("Download"));
          gtk_widget_set_sensitive (GTK_WIDGET (button), TRUE);
        }

      if (dialog != NULL && error != NULL && !nuvashalauncher_error_is_cancelled (error))
        {
          const char *msg = nuvashalauncher_error_message (error);
          show_toast (dialog, msg);
        }
    }

  install_job_free (job);
}

typedef struct {
  NuvashaLauncherJavaDownloaderDialog *dialog; /* borrowed; row lives inside dialog */
  NuvashaLauncherJavaRelease *release;
  GtkButton *button;
  GtkWidget *spinner;
} RowActionData;

static void
row_action_data_free (gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  RowActionData *action = data;

  if (action == NULL)
    return;

  nuvashalauncher_java_release_free (action->release);
  g_free (action);
}

static void
on_download_clicked (GtkButton *button, gpointer user_data)
{
  RowActionData *action = user_data;
  NuvashaLauncherJavaDownloaderDialog *self = action->dialog;
  g_autofree char *dir_name =
    nuvashalauncher_java_download_install_dir_name (action->release);
  g_autofree char *title = NULL;
  g_autoptr (NuvashaLauncherJavaRow) java_row = g_weak_ref_get (&self->java_row_ref);
  InstallJob *job;
  NuvashaLauncherTaskScope *scope;

  if (g_hash_table_contains (get_active_installs (), dir_name))
    return;

  g_hash_table_add (get_active_installs (), g_strdup (dir_name));

  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_button_set_label (button, _("Downloading…"));
  gtk_widget_set_visible (action->spinner, TRUE);

  title = g_strdup_printf (_("Eclipse Temurin %d"), action->release->major_version);

  job = g_new0 (InstallJob, 1);
  job->release = nuvashalauncher_java_release_copy (action->release);
  job->dir_name = g_steal_pointer (&dir_name);
  job->download = g_object_ref (
    nuvashalauncher_downloads_add (NUVASHALAUNCHER_DOWNLOAD_JAVA, title));
  nuvashalauncher_download_set_detail (job->download, _("Downloading…"));
  g_weak_ref_init (&job->dialog_ref, self);
  g_weak_ref_init (&job->button_ref, button);
  g_weak_ref_init (&job->spinner_ref, action->spinner);
  g_weak_ref_init (&job->java_row_ref, java_row);

  scope = nuvashalauncher_java_download_install_async (
    nuvashalauncher_downloads_get_service (),
    action->release,
    NULL,
    on_install_progress,
    on_install_done,
    job);
  nuvashalauncher_task_scope_release_when_done (
    scope, nuvashalauncher_download_get_cancellable (job->download));
}

static void
clear_release_rows (NuvashaLauncherJavaDownloaderDialog *self)
{
  if (self->rows == NULL)
    return;

  for (guint i = 0; i < self->rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (self->rows, i);
      adw_preferences_group_remove (self->releases_group, row);
    }
  g_ptr_array_set_size (self->rows, 0);
}

static GtkWidget *
build_release_row (NuvashaLauncherJavaDownloaderDialog *self,
                   const NuvashaLauncherJavaRelease *release)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  g_autofree char *title = NULL;
  g_autofree char *subtitle = NULL;
  g_autofree char *dir_name = nuvashalauncher_java_download_install_dir_name (release);
  gboolean installed = nuvashalauncher_java_download_is_installed (release, NULL);
  gboolean downloading = g_hash_table_contains (get_active_installs (), dir_name);
  GtkWidget *spinner = GTK_WIDGET (adw_spinner_new ());
  GtkButton *button = GTK_BUTTON (gtk_button_new ());
  RowActionData *action;

  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);

  if (release->lts)
    title = g_strdup_printf (_("Java %d (LTS)"), release->major_version);
  else
    title = g_strdup_printf (_("Java %d"), release->major_version);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);

  if (release->size > 0)
    {
      g_autofree char *size_str = g_format_size (release->size);
      subtitle = g_strdup_printf ("%s · %s", release->version_display, size_str);
    }
  else
    {
      subtitle = g_strdup (release->version_display);
    }
  adw_action_row_set_subtitle (row, subtitle);

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

  action = g_new0 (RowActionData, 1);
  action->dialog = self;
  action->release = nuvashalauncher_java_release_copy (release);
  action->button = button;
  action->spinner = spinner;
  g_signal_connect_data (button, "clicked",
                         G_CALLBACK (on_download_clicked),
                         action, row_action_data_free, 0);

  adw_action_row_add_suffix (row, GTK_WIDGET (button));
  return GTK_WIDGET (row);
}

static void
on_releases_loaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  NuvashaLauncherJavaDownloaderDialog *self =
    NUVASHALAUNCHER_JAVA_DOWNLOADER_DIALOG (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) releases =
    nuvashalauncher_java_download_list_finish (result, &error);

  g_clear_pointer (&self->list_scope, nuvashalauncher_task_scope_unref);

  if (self->stack == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (releases == NULL)
    {
      if (error != NULL && !nuvashalauncher_error_is_cancelled (error))
        {
          const char *msg = nuvashalauncher_error_message (error);
          adw_status_page_set_description (self->error_page, msg);
          gtk_stack_set_visible_child_name (self->stack, "error");
        }
      g_object_unref (self);
      return;
    }

  clear_release_rows (self);

  if (releases->len == 0)
    {
      gtk_stack_set_visible_child_name (self->stack, "empty");
      g_object_unref (self);
      return;
    }

  for (guint i = 0; i < releases->len; i++)
    {
      const NuvashaLauncherJavaRelease *release = g_ptr_array_index (releases, i);
      GtkWidget *row = build_release_row (self, release);
      adw_preferences_group_add (self->releases_group, row);
      g_ptr_array_add (self->rows, row);
    }

  gtk_stack_set_visible_child_name (self->stack, "list");
  g_object_unref (self);
}

static void
load_releases (NuvashaLauncherJavaDownloaderDialog *self)
{
  if (self->list_scope != NULL)
    {
      nuvashalauncher_task_scope_cancel (self->list_scope);
      g_clear_pointer (&self->list_scope, nuvashalauncher_task_scope_unref);
    }

  gtk_stack_set_visible_child_name (self->stack, "loading");

  self->list_scope = nuvashalauncher_java_download_list_async (
    nuvashalauncher_downloads_get_service (),
    NULL,
    on_releases_loaded,
    g_object_ref (self));
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  load_releases (NUVASHALAUNCHER_JAVA_DOWNLOADER_DIALOG (user_data));
}

static void
nuvashalauncher_java_downloader_dialog_dispose (GObject *object)
{
  NuvashaLauncherJavaDownloaderDialog *self =
    NUVASHALAUNCHER_JAVA_DOWNLOADER_DIALOG (object);

  if (self->list_scope != NULL)
    {
      nuvashalauncher_task_scope_cancel (self->list_scope);
      g_clear_pointer (&self->list_scope, nuvashalauncher_task_scope_unref);
    }

  g_clear_pointer (&self->rows, g_ptr_array_unref);
  g_weak_ref_clear (&self->java_row_ref);

  gtk_widget_dispose_template (GTK_WIDGET (self),
                               NUVASHALAUNCHER_TYPE_JAVA_DOWNLOADER_DIALOG);

  G_OBJECT_CLASS (nuvashalauncher_java_downloader_dialog_parent_class)->dispose (object);
}

static void
nuvashalauncher_java_downloader_dialog_class_init (
  NuvashaLauncherJavaDownloaderDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = nuvashalauncher_java_downloader_dialog_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               JAVA_DOWNLOADER_DIALOG_RESOURCE);
  gtk_widget_class_bind_template_child (widget_class,
                                        NuvashaLauncherJavaDownloaderDialog,
                                        toast_overlay);
  gtk_widget_class_bind_template_child (widget_class,
                                        NuvashaLauncherJavaDownloaderDialog,
                                        stack);
  gtk_widget_class_bind_template_child (widget_class,
                                        NuvashaLauncherJavaDownloaderDialog,
                                        error_page);
  gtk_widget_class_bind_template_child (widget_class,
                                        NuvashaLauncherJavaDownloaderDialog,
                                        retry_button);
  gtk_widget_class_bind_template_child (widget_class,
                                        NuvashaLauncherJavaDownloaderDialog,
                                        releases_group);
}

static void
nuvashalauncher_java_downloader_dialog_init (
  NuvashaLauncherJavaDownloaderDialog *self)
{
  const char *arch = nuvashalauncher_java_download_host_arch ();

  gtk_widget_init_template (GTK_WIDGET (self));

  self->rows = g_ptr_array_new ();
  g_weak_ref_init (&self->java_row_ref, NULL);

  if (arch != NULL)
    {
      g_autofree char *desc = g_strdup_printf (_("Linux (%s)"), arch);
      adw_preferences_group_set_description (self->releases_group, desc);
    }

  g_signal_connect_object (self->retry_button, "clicked",
                           G_CALLBACK (on_retry_clicked), self, 0);
}

NuvashaLauncherJavaDownloaderDialog *
nuvashalauncher_java_downloader_dialog_new (NuvashaLauncherJavaRow *java_row)
{
  NuvashaLauncherJavaDownloaderDialog *self =
    g_object_new (NUVASHALAUNCHER_TYPE_JAVA_DOWNLOADER_DIALOG, NULL);

  g_weak_ref_set (&self->java_row_ref, java_row);
  load_releases (self);
  return self;
}
