/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-index.h"
#include "nuvashalauncher-content-files-private.h"
#include "nuvashalauncher-operation.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-i18n.h"

/* Single and bulk deletion of installed content, including dependent cleanup. */

static void
remove_installed_file_by_path (NuvashaLauncherContentPage *self, const char *target_path)
{
  if (target_path == NULL || self->installed.files == NULL)
    return;

  if (self->instance != NULL)
    {
      g_autofree char *directory = NULL;
      g_autofree char *filename = g_path_get_basename (target_path);

      g_object_get (self->instance, "directory", &directory, NULL);
      if (directory != NULL)
        nuvashalauncher_operation_unref_when_done (
          nuvashalauncher_content_index_remove_async (directory, self->type,
                                                      filename, NULL, NULL));
    }

  if (self->installed.selected_paths != NULL)
    g_hash_table_remove (self->installed.selected_paths, target_path);

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      NuvashaLauncherContentFile *file =
        g_ptr_array_index (self->installed.files, i);

      if (g_strcmp0 (file->path, target_path) == 0)
        {
          GtkWidget *row = self->installed.rows != NULL
                             ? GTK_WIDGET (g_hash_table_lookup (
                                 self->installed.rows, target_path))
                             : NULL;
          GList *link = g_list_find (self->installed.rows_list, row);

          if (row != NULL)
            {
              g_object_set_data (G_OBJECT (row), "nuvashalauncher-file", NULL);
              if (self->installed.group != NULL)
                adw_preferences_group_remove (
                  ADW_PREFERENCES_GROUP (self->installed.group), row);
              g_hash_table_remove (self->installed.rows, target_path);
            }

          if (link != NULL)
            {
              self->installed.rows_list =
                g_list_delete_link (self->installed.rows_list, link);
              g_object_unref (row);
            }

          g_ptr_array_remove_index (self->installed.files, i);
          if (self->installed.status != NULL)
            gtk_widget_set_visible (GTK_WIDGET (self->installed.status),
                                    self->installed.files->len == 0);
          if (self->installed.scroller != NULL)
            gtk_widget_set_visible (GTK_WIDGET (self->installed.scroller),
                                    self->installed.files->len != 0);
          break;
        }
    }
}

static void
on_deleted (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  const char *path = g_object_get_data (G_OBJECT (source), "nuvashalauncher-path");
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_content_delete_finish (G_FILE (source), result, &error))
    show_error (self, _("The file cannot be deleted"), error);
  else if (self->installed.files != NULL)
    {
      g_autoptr (GPtrArray) dependent_paths =
        g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GPtrArray) removed_dependents =
        g_ptr_array_new_with_free_func (g_free);
      g_autoptr (NuvashaLauncherSettings) settings = nuvashalauncher_settings_new ();
      NuvashaLauncherContentFile *deleted = NULL;

      for (guint i = 0; i < self->installed.files->len; i++)
        {
          NuvashaLauncherContentFile *candidate =
            g_ptr_array_index (self->installed.files, i);

          if (g_strcmp0 (candidate->path, path) == 0)
            {
              deleted = candidate;
              break;
            }
        }

      if (deleted != NULL && deleted->project_id != NULL)
        for (guint i = 0; i < self->installed.files->len; i++)
          {
            NuvashaLauncherContentFile *other =
              g_ptr_array_index (self->installed.files, i);

            if (other == deleted || other->required_dependencies == NULL)
              continue;

            for (guint j = 0; j < other->required_dependencies->len; j++)
              if (g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                             deleted->project_id) == 0 ||
                  g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                             deleted->local_id) == 0)
                {
                  g_ptr_array_add (dependent_paths, g_strdup (other->path));
                  break;
                }
          }

      for (guint i = 0; i < dependent_paths->len; i++)
        {
          const char *dependent = g_ptr_array_index (dependent_paths, i);
          g_autoptr (GError) dependent_error = NULL;
          gboolean changed = FALSE;

          if (nuvashalauncher_settings_get_delete_dependents (settings))
            {
              changed = nuvashalauncher_content_remove_file (dependent, &dependent_error);
              if (changed)
                g_ptr_array_add (removed_dependents, g_strdup (dependent));
            }
          else
            {
              NuvashaLauncherContentFile *dependent_file = NULL;
              GtkWidget *dependent_row = NULL;
              for (guint j = 0; j < self->installed.files->len; j++)
                {
                  NuvashaLauncherContentFile *candidate =
                    g_ptr_array_index (self->installed.files, j);

                  if (g_strcmp0 (candidate->path, dependent) == 0)
                    {
                      dependent_file = candidate;
                      break;
                    }
                }

              if (dependent_file != NULL)
                {
                  /* Use the same operation as a user pressing the switch.
                   * In particular, this performs the filesystem rename and
                   * updates the file and row maps together. */
                  changed = !dependent_file->enabled ||
                            nuvashalauncher_content_page_toggle_file (self, dependent_file);
                  dependent_row = g_hash_table_lookup (self->installed.rows,
                                                       dependent_file->path);

                  if (dependent_row != NULL)
                    {
                      GtkSwitch *switch_widget = GTK_SWITCH (
                        g_object_get_data (G_OBJECT (dependent_row),
                                           "nuvashalauncher-mod-switch"));
                      if (switch_widget != NULL)
                        {
                          gulong handler = GPOINTER_TO_SIZE (
                            g_object_get_data (G_OBJECT (switch_widget),
                                               "nuvashalauncher-toggle-handler"));

                          if (handler != 0)
                            g_signal_handler_block (switch_widget, handler);
                          gtk_switch_set_active (switch_widget, FALSE);
                          if (handler != 0)
                            g_signal_handler_unblock (switch_widget, handler);
                          gtk_widget_set_tooltip_text (
                            GTK_WIDGET (switch_widget), _("Enable"));
                        }
                    }
                }
            }

          if (!changed)
            {
              show_error (self,
                          nuvashalauncher_settings_get_delete_dependents (settings)
                            ? _("A dependent mod cannot be deleted")
                            : _("The file cannot be renamed"),
                          dependent_error);
              break;
            }
        }

      show_toast (self, _("Deleted"));
      /* An identification request may still refer to one of the rows. Stop
         it before releasing the file it borrowed; the other rows remain in
         place and are not rebuilt. */
      if (self->installed.scan != NULL)
        {
          g_cancellable_cancel (self->installed.scan);
          g_clear_object (&self->installed.scan);
          self->installed.scan = g_cancellable_new ();
        }
      self->installed.identifying = FALSE;

      for (guint i = 0; i < removed_dependents->len; i++)
        remove_installed_file_by_path (self,
                                       g_ptr_array_index (removed_dependents, i));
      remove_installed_file_by_path (self, path);
    }

  g_object_unref (self);
}

/* Handle the confirmation result. The path was copied onto the
 * alert, so a list that is read again while it is up cannot leave it
 * pointing at a file that is gone. */
static void
on_delete_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  const char *path = g_object_get_data (G_OBJECT (source), "nuvashalauncher-path");
  GPtrArray *dependent_paths =
    g_object_get_data (G_OBJECT (source), "nuvashalauncher-dependent-paths");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
  g_autoptr (GFile) target = NULL;

  if (path == NULL || g_strcmp0 (response, "confirm") != 0)
    {
      g_object_unref (self);
      return;
    }

  target = g_file_new_for_path (path);
  g_object_set_data_full (G_OBJECT (target), "nuvashalauncher-path",
                          g_strdup (path), g_free);
  g_object_set_data_full (G_OBJECT (target), "nuvashalauncher-dependent-paths",
                          g_ptr_array_ref (dependent_paths),
                          (GDestroyNotify) g_ptr_array_unref);
  /* Deleting is a system call on the file and goes through GIO off the
     main loop; the ref on the page travels to on_deleted(). */
  nuvashalauncher_content_delete_async (target, G_PRIORITY_DEFAULT, NULL, on_deleted,
                                        self);
}

/* The trash button asks first, then deletes the file for good. */
void
nuvashalauncher_content_page_on_trash_clicked (GtkButton *button G_GNUC_UNUSED,
                                               gpointer   user_data)
{
  NuvashaLauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (button), "nuvashalauncher-file");
  g_autofree char *heading = NULL;
  g_autofree char *body = NULL;
  g_autoptr (GString) dependents = g_string_new (NULL);
  g_autoptr (GPtrArray) dependent_paths =
    g_ptr_array_new_with_free_func (g_free);
  const char *title = NULL;
  AdwAlertDialog *alert = NULL;

  if (file == NULL)
    return;

  title = nuvashalauncher_content_page_file_display_name (file);
  if (title == NULL || *title == '\0')
    title = file->filename;

  if (file->project_id != NULL)
    for (guint i = 0; i < self->installed.files->len; i++)
      {
        NuvashaLauncherContentFile *other = g_ptr_array_index (self->installed.files, i);
        const char *other_title = NULL;

        if (other == file || other->required_dependencies == NULL)
          continue;

        for (guint j = 0; j < other->required_dependencies->len; j++)
          if (g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                         file->project_id) == 0 ||
              g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                         file->local_id) == 0)
            {
              other_title = nuvashalauncher_content_page_file_display_name (other);
              if (dependents->len > 0)
                g_string_append_c (dependents, '\n');
              g_string_append (dependents, other_title);
              g_ptr_array_add (dependent_paths, g_strdup (other->path));
              break;
            }
      }

  heading = g_strdup_printf (_("Delete \"%s\"?"), title);
  if (dependents->len > 0)
    body = g_strdup_printf (
      _("The following mods depend on %s:\n%s"), title, dependents->str);
  else
    body = NULL;

  alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body));
  if (body == NULL)
    adw_alert_dialog_set_body (alert, "");
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "confirm", _("Delete"));
  adw_alert_dialog_set_response_appearance (alert, "confirm",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  g_object_set_data_full (G_OBJECT (alert), "nuvashalauncher-path",
                          g_strdup (file->path), g_free);
  g_object_set_data_full (G_OBJECT (alert), "nuvashalauncher-dependent-paths",
                          g_steal_pointer (&dependent_paths),
                          (GDestroyNotify) g_ptr_array_unref);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_delete_chosen,
                           g_object_ref (self));
}

static GPtrArray *
content_file_dependents (NuvashaLauncherContentPage *self,
                         NuvashaLauncherContentFile *file,
                         GString                    *names)
{
  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

  if (file->project_id == NULL)
    return paths;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      NuvashaLauncherContentFile *other = g_ptr_array_index (self->installed.files, i);
      const char *other_title;

      if (other == file || other->required_dependencies == NULL)
        continue;
      for (guint j = 0; j < other->required_dependencies->len; j++)
        if (g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                       file->project_id) == 0 ||
            g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                       file->local_id) == 0)
          {
            other_title = nuvashalauncher_content_page_file_display_name (other);
            if (names != NULL)
              {
                if (names->len > 0)
                  g_string_append_c (names, '\n');
                g_string_append (names, other_title);
              }
            g_ptr_array_add (paths, g_strdup (other->path));
            break;
          }
    }

  return paths;
}

static void
on_bulk_content_delete_chosen (GObject *source, GAsyncResult *result,
                               gpointer user_data)
{
  NuvashaLauncherContentPage *self = user_data;
  const char *response = adw_alert_dialog_choose_finish (
    ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "confirm") == 0)
    {
      GHashTableIter iter;
      gpointer key;
      g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);

      g_hash_table_iter_init (&iter, self->installed.selected_paths);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        g_ptr_array_add (paths, g_strdup (key));

      g_hash_table_remove_all (self->installed.selected_paths);
      content_page_toggle_edit (NULL, self);

      for (guint i = 0; i < paths->len; i++)
        {
          const char *path = g_ptr_array_index (paths, i);
          NuvashaLauncherContentFile *file = NULL;
          g_autoptr (GFile) target = NULL;
          g_autoptr (GPtrArray) dependents = NULL;

          for (guint j = 0; j < self->installed.files->len; j++)
            {
              NuvashaLauncherContentFile *candidate =
                g_ptr_array_index (self->installed.files, j);
              if (g_strcmp0 (candidate->path, path) == 0)
                {
                  file = candidate;
                  break;
                }
            }
          if (file == NULL)
            continue;

          dependents = content_file_dependents (self, file, NULL);
          target = g_file_new_for_path (path);
          g_object_set_data_full (G_OBJECT (target), "nuvashalauncher-path",
                                  g_strdup (path), g_free);
          g_object_set_data_full (G_OBJECT (target), "nuvashalauncher-dependent-paths",
                                  g_ptr_array_ref (dependents),
                                  (GDestroyNotify) g_ptr_array_unref);
          nuvashalauncher_content_delete_async (target, G_PRIORITY_DEFAULT, NULL,
                                                on_deleted, g_object_ref (self));
        }
    }

  g_object_unref (self);
}

void
content_page_bulk_delete (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherContentPage *self = user_data;
  GHashTableIter iter;
  gpointer key;
  GString *names = g_string_new (NULL);
  GString *dependencies = g_string_new (NULL);
  g_autofree char *heading = NULL;
  g_autofree char *body = NULL;
  g_autoptr (NuvashaLauncherSettings) settings = nuvashalauncher_settings_new ();
  gboolean include_dependencies =
    nuvashalauncher_settings_get_delete_dependents (settings);

  g_hash_table_iter_init (&iter, self->installed.selected_paths);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      NuvashaLauncherContentFile *file = NULL;
      for (guint i = 0; i < self->installed.files->len; i++)
        {
          NuvashaLauncherContentFile *candidate =
            g_ptr_array_index (self->installed.files, i);
          if (g_strcmp0 (candidate->path, key) == 0)
            {
              file = candidate;
              break;
            }
        }
      if (file == NULL)
        continue;
      if (names->len > 0)
        g_string_append_c (names, '\n');
      g_string_append (names, nuvashalauncher_content_page_file_display_name (file));
      if (include_dependencies)
        {
          g_autoptr (GPtrArray) unused =
            content_file_dependents (self, file, dependencies);
        }
    }

  const char *kind = self->type == nuvashalauncher_CONTENT_MOD ? "mods" :
                     self->type == nuvashalauncher_CONTENT_SHADER_PACK ? "shaderpacks" :
                     self->type == nuvashalauncher_CONTENT_RESOURCE_PACK ? "resourcepacks" :
                     "datapacks";
  heading = g_strdup_printf (_("Delete selected %s?"), kind);
  if (dependencies->len > 0)
    body = g_strdup_printf (_("Selected %s:\n%s\n\nDependencies:\n%s"),
                            nuvashalauncher_content_type_title (self->type),
                            names->str, dependencies->str);
  else
    body = g_strdup_printf (_("Selected %s:\n%s"),
                            nuvashalauncher_content_type_title (self->type), names->str);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body));
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "confirm", _("Delete"));
  adw_alert_dialog_set_response_appearance (alert, "confirm",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_bulk_content_delete_chosen, g_object_ref (self));
  g_string_free (names, TRUE);
  g_string_free (dependencies, TRUE);
}
