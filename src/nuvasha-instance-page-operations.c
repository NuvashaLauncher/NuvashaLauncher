/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Directory, duplicate, delete and export operations for an instance page. */

#include "nuvasha-instance-page-private.h"
#include "nuvasha-i18n.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-modpack.h"
#include "nuvasha-operation.h"
#include "nuvasha-ui-feedback.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

/* ---------------------------------------------------------- *
 * Open folder, duplicate, delete
 * ---------------------------------------------------------- */

typedef struct
{
  NuvashaInstancePage *page;
  NuvashaTaskScope *operation;
} OpenFolderWait;

static void
on_open_folder_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OpenFolderWait *wait = user_data;
  NuvashaInstancePage *self = wait->page;
  NuvashaTaskScope *operation = wait->operation;
  g_autoptr (GError) error = NULL;

  if (!nuvasha_instance_manager_open_folder_finish (nuvasha_INSTANCE_MANAGER (source),
                                                result, &error))
    instance_page_show_toast (self, error->message);

  g_object_unref (self);
  nuvasha_operation_unref (operation);
  g_free (wait);
}

void
on_open_folder_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);

  OpenFolderWait *wait = g_new0 (OpenFolderWait, 1);
  wait->page = g_object_ref (self);
  wait->operation =
    nuvasha_instance_manager_open_folder_async (self->manager, self->instance,
                                            on_open_folder_ready, wait);
}

static void
on_export_progress (guint64 done, guint64 total, const char *detail,
                    gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);

  if (self->export_download != NULL)
    {
      nuvasha_download_set_detail (self->export_download, detail);
      nuvasha_download_set_progress (self->export_download, done, total);
    }
}

static void
on_export_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  g_autoptr (GError) error = NULL;

  if (!nuvasha_modpack_export_finish (result, &error))
    instance_page_show_toast (self, error->message);
  else
    instance_page_show_toast (self, _("Modpack exported"));

  if (self->export_download != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->export_download),
                                    (gpointer *) &self->export_download);
      nuvasha_download_finish (self->export_download);
      self->export_download = NULL;
    }
  if (self->export_operation != NULL)
    {
      nuvasha_operation_unref (self->export_operation);
      self->export_operation = NULL;
    }

  g_object_unref (self);
}

static void
on_export_file_chosen (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED))
        instance_page_show_toast (self, error->message);
      g_object_unref (self);
      return;
    }

  {
    g_autofree char *path = g_file_get_path (file);

    if (path == NULL)
      instance_page_show_toast (self, _("The export location is not a local file"));
    else
      {
        g_autofree char *name = instance_string (self->instance, "name");

        self->export_download =
          nuvasha_downloads_add (nuvasha_DOWNLOAD_UPDATE,
                             name != NULL ? name : _("Modpack export"));
        nuvasha_download_set_detail (self->export_download,
                                 _("Exporting modpack"));
        g_object_add_weak_pointer (G_OBJECT (self->export_download),
                                   (gpointer *) &self->export_download);
        self->export_operation = nuvasha_modpack_export_async (
          self->instance, path, self->export_name, self->export_summary,
          self->export_version, self->export_include_transient,
          on_export_progress, self, on_export_done, g_object_ref (self));

        /* The entry's × is the cancel button the user can see, so the
           export follows that entry's cancellable. */
        nuvasha_operation_follow (
          self->export_operation,
          nuvasha_download_get_cancellable (self->export_download));
      }
  }

  g_object_unref (self);
}

static void
open_export_file_dialog (NuvashaInstancePage *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_autofree char *name = instance_string (self->instance, "name");
  g_autofree char *filename = g_strdup_printf (
    "%s.mrpack", name != NULL ? name : "instance");

  gtk_file_dialog_set_title (dialog, _("Export Modpack"));
  gtk_file_dialog_set_initial_name (dialog, filename);
  gtk_file_filter_set_name (filter, _("Modrinth Modpacks (*.mrpack)"));
  gtk_file_filter_add_suffix (filter, "mrpack");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_save (dialog,
                        GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, on_export_file_chosen, g_object_ref (self));
}

static void show_export_policy (NuvashaInstancePage *self);

typedef struct {
  NuvashaInstancePage *page;
  AdwDialog *dialog;
  AdwEntryRow *name;
  AdwEntryRow *summary;
  AdwEntryRow *version;
} ExportMetadata;

static void
export_metadata_free (ExportMetadata *data)
{
  g_object_unref (data->page);
  g_object_unref (data->dialog);
  g_free (data);
}

static gboolean
export_metadata_free_idle (gpointer user_data)
{
  ExportMetadata *data = user_data;
  nuvasha_operation_forget_source (data->page->scope, data->page->export_idle);
  data->page->export_idle = 0;
  return G_SOURCE_REMOVE;
}

static void
on_export_metadata_closed (AdwDialog *dialog G_GNUC_UNUSED,
                           gpointer user_data)
{
  /* Adwaita is still completing its close signal while this callback runs.
   * Release the dialog after the close sequence has finished. */
  ExportMetadata *data = user_data;
  data->page->export_idle = g_idle_add_full (
    G_PRIORITY_DEFAULT, export_metadata_free_idle, data,
    (GDestroyNotify) export_metadata_free);
  nuvasha_operation_track (data->page->scope, data->page->export_idle);
}

static void
on_export_metadata_continue (GtkButton *button G_GNUC_UNUSED,
                             gpointer user_data)
{
  ExportMetadata *data = user_data;
  NuvashaInstancePage *self = data->page;

  g_free (self->export_name);
  g_free (self->export_summary);
  g_free (self->export_version);
  self->export_name = g_strdup (gtk_editable_get_text (
    GTK_EDITABLE (data->name)));
  self->export_summary = g_strdup (gtk_editable_get_text (
    GTK_EDITABLE (data->summary)));
  self->export_version = g_strdup (gtk_editable_get_text (
    GTK_EDITABLE (data->version)));

  adw_dialog_close (data->dialog);

  /* The transient-file choice remains a separate, deliberately simple
   * safety decision immediately before choosing the destination. */
  show_export_policy (self);
}

static void
on_export_metadata_cancel (GtkButton *button G_GNUC_UNUSED,
                           gpointer user_data)
{
  ExportMetadata *data = user_data;

  adw_dialog_close (data->dialog);
}

static void
show_export_metadata (NuvashaInstancePage *self)
{
  ExportMetadata *data = g_new0 (ExportMetadata, 1);
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkLabel *heading = GTK_LABEL (gtk_label_new (_("Pack information")));
  AdwPreferencesGroup *fields = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkButton *cancel = GTK_BUTTON (gtk_button_new_with_label (_("Cancel")));
  GtkButton *export_button = GTK_BUTTON (gtk_button_new_with_label (_("Export")));
  g_autofree char *name = instance_string (self->instance, "name");

  data->page = g_object_ref (self);
  data->dialog = ADW_DIALOG (g_object_ref_sink (adw_dialog_new ()));
  data->name = ADW_ENTRY_ROW (adw_entry_row_new ());
  data->summary = ADW_ENTRY_ROW (adw_entry_row_new ());
  data->version = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_dialog_set_title (data->dialog, _("Export modpack metadata"));
  adw_dialog_set_follows_content_size (data->dialog, FALSE);
  g_signal_connect (data->dialog, "closed",
                    G_CALLBACK (on_export_metadata_closed), data);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (data->name),
                                  _("Pack Name"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (data->summary),
                                  _("Summary"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (data->version),
                                  _("Version ID"));
  gtk_widget_add_css_class (GTK_WIDGET (heading), "title-2");
  gtk_label_set_justify (GTK_LABEL (heading), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (heading), 0.5f);
  gtk_widget_set_margin_bottom (GTK_WIDGET (heading), 12);
  gtk_editable_set_text (GTK_EDITABLE (data->name), name != NULL ? name : "");
  gtk_editable_set_text (GTK_EDITABLE (data->version), "");
  adw_preferences_group_add (fields,
                             GTK_WIDGET (data->name));
  adw_preferences_group_add (fields,
                             GTK_WIDGET (data->summary));
  adw_preferences_group_add (fields,
                             GTK_WIDGET (data->version));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (heading));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (fields));
  gtk_widget_set_margin_start (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (box), 12);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (buttons));
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (cancel));
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (export_button));
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (export_button), "suggested-action");
  g_signal_connect (cancel, "clicked",
                    G_CALLBACK (on_export_metadata_cancel), data);
  g_signal_connect (export_button, "clicked",
                    G_CALLBACK (on_export_metadata_continue), data);
  adw_dialog_set_child (data->dialog, GTK_WIDGET (box));
  adw_dialog_present (data->dialog, GTK_WIDGET (self));
}

static void
on_export_policy_chosen (GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "cancel") != 0)
    {
      self->export_include_transient = g_strcmp0 (response, "include") == 0;
      open_export_file_dialog (self);
    }

  g_object_unref (self);
}

void
on_export_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);

  if (self->export_operation != NULL)
    {
      instance_page_show_toast (self, _("Exporting modpack"));
      return;
    }

  if (nuvasha_settings_get_export_metadata (self->settings))
    show_export_metadata (self);
  else
    {
      g_clear_pointer (&self->export_name, g_free);
      g_clear_pointer (&self->export_summary, g_free);
      g_clear_pointer (&self->export_version, g_free);
      show_export_policy (self);
    }
}

static void
show_export_policy (NuvashaInstancePage *self)
{
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (_("Include transient files?"),
                          _("Logs, crash reports, screenshots, caches and "
                            "downloads are normally excluded.")));

  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "clean", _("Clean Export"));
  adw_alert_dialog_add_response (alert, "include", _("Include All Files"));
  adw_alert_dialog_set_response_appearance (alert, "include",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "clean");
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_export_policy_chosen, g_object_ref (self));
}

/* ---------------------------------------------------------- *
 * Changing the directory
 * ---------------------------------------------------------- */

/* The row says where the instance is, and whether it may be moved right
 * now. A running game has its files open where they are, so the button
 * does nothing until it has stopped. */
void
update_directory_row (NuvashaInstancePage *self)
{
  gboolean running =
    nuvasha_launch_state_is_busy (nuvasha_launcher_get_state (self->launcher));

  if (self->directory_row == NULL)
    return;

  if (self->directory_path_label != NULL)
    {
      g_autofree char *directory =
        instance_string (self->instance, "directory");

      gtk_label_set_text (GTK_LABEL (self->directory_path_label), directory);
    }

  gtk_widget_set_sensitive (GTK_WIDGET (self->directory_change), !running);
}

static void
set_moving (NuvashaInstancePage *self, gboolean moving)
{
  gtk_widget_set_visible (GTK_WIDGET (self->directory_spinner), moving);
  gtk_widget_set_sensitive (GTK_WIDGET (self->directory_change), !moving);
}

static void
on_change_directory_ready (GObject *source, GAsyncResult *result,
                            gpointer user_data);

typedef struct
{
  NuvashaInstancePage *page;
  NuvashaTaskScope *operation;
} ChangeDirectoryWait;

static void
on_change_directory_ready (GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
  ChangeDirectoryWait *wait = user_data;
  NuvashaInstancePage *self = wait->page;
  g_autoptr (GError) error = NULL;

  if (nuvasha_instance_manager_change_directory_finish (
        nuvasha_INSTANCE_MANAGER (source), result, &error))
    {
      /* The instance stores the new path; the row reads it from there, so
         what it says is what the file says. */
      update_directory_row (self);
    }
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      instance_page_show_toast (self, error->message);
    }

  set_moving (self, FALSE);
  update_directory_row (self);

  g_object_unref (self);
  nuvasha_operation_unref (wait->operation);
  g_free (wait);
}

static void
on_directory_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), result,
                                          &error);

  /* The chooser being dismissed is not a failure and there is nothing to
     report for it. */
  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED))
        instance_page_show_toast (self, error->message);

      g_object_unref (self);
      return;
    }

  {
    g_autofree char *path = g_file_get_path (file);
    g_autofree char *canonical = NULL;

    /* Said before anything is touched, so a path that cannot work is
       refused with its reason and the instance stays as it was. */
    if (!nuvasha_instance_manager_check_new_directory (self->manager,
                                                   self->instance, path,
                                                   &canonical, &error))
      {
        instance_page_show_toast (self, error->message);
        g_object_unref (self);
        return;
      }

    set_moving (self, TRUE);

    ChangeDirectoryWait *wait = g_new0 (ChangeDirectoryWait, 1);
    wait->page = g_object_ref (self);
    wait->operation = nuvasha_instance_manager_change_directory_async (
      self->manager, self->instance, canonical, on_change_directory_ready,
      wait);
  }

  g_object_unref (self);
}

void
on_change_directory_clicked (GtkButton *button G_GNUC_UNUSED,
                             gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();

  /* A game that is running has its files where the instance says they
     are; moving them out from under it would break the game and the move
     at the same time. */
  if (nuvasha_launch_state_is_busy (nuvasha_launcher_get_state (self->launcher)))
    {
      instance_page_show_toast (self, _("The game is running; stop it before moving its "
                          "directory"));
      return;
    }

  gtk_file_dialog_set_title (dialog, _("Choose a Directory"));
  gtk_file_dialog_select_folder (dialog,
                                 GTK_IS_WINDOW (root) ? GTK_WINDOW (root)
                                                      : NULL,
                                 NULL, on_directory_chosen,
                                 g_object_ref (self));
}

static void
on_duplicate_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ChangeDirectoryWait *wait = user_data;
  NuvashaInstancePage *self = wait->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstance) copy =
    nuvasha_instance_manager_duplicate_finish (nuvasha_INSTANCE_MANAGER (source),
                                           result, &error);

  if (copy == NULL)
    instance_page_show_toast (self, error->message);
  else
    {
      g_autofree char *name = instance_string (copy, "name");
      g_autofree char *message =
        g_strdup_printf (_("Duplicated to \"%s\""), name);

      instance_page_show_toast (self, message);
    }

  g_object_unref (self);
  nuvasha_operation_unref (wait->operation);
  g_free (wait);
}

void
on_duplicate_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);

  ChangeDirectoryWait *wait = g_new0 (ChangeDirectoryWait, 1);
  wait->page = g_object_ref (self);
  wait->operation = nuvasha_instance_manager_duplicate_async (
    self->manager, self->instance, on_duplicate_ready, wait);
}

static void
on_delete_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ChangeDirectoryWait *wait = user_data;
  NuvashaInstancePage *self = wait->page;
  g_autoptr (GError) error = NULL;

  if (!nuvasha_instance_manager_delete_finish (nuvasha_INSTANCE_MANAGER (source),
                                           result, &error))
    {
      /* The instance is still registered: stay on its page. */
      instance_page_show_toast (self, error->message);
      g_object_unref (self);
      nuvasha_operation_unref (wait->operation);
      g_free (wait);
      return;
    }

  /* Nothing more to do here: the instance left the list, and the window
     closes the tab of an instance that is no longer in it. Asking this
     page to close itself would mean two places deciding the same thing. */

  g_object_unref (self);
  nuvasha_operation_unref (wait->operation);
  g_free (wait);
}

static void
on_delete_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "delete") == 0 && self->manager != NULL)
     {
       ChangeDirectoryWait *wait = g_new0 (ChangeDirectoryWait, 1);
       wait->page = g_object_ref (self);
       wait->operation = nuvasha_instance_manager_delete_async (
         self->manager, self->instance, TRUE, on_delete_ready, wait);
     }

  g_object_unref (self);
}

void
on_delete_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  g_autofree char *name = instance_string (self->instance, "name");
  g_autofree char *heading = g_strdup_printf (_("Delete \"%s\"?"), name);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (heading, NULL));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);

  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "delete", _("Delete"));
  adw_alert_dialog_set_response_appearance (alert, "delete",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_delete_chosen,
                           g_object_ref (self));
}
