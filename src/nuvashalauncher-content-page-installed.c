/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-index.h"
#include "nuvashalauncher-content-format.h"
#include "nuvashalauncher-content-files-private.h"
#include "nuvashalauncher-operation.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-i18n.h"

/* Installed-content rows, selection, deletion, and icon loading. */

void
show_error (nuvashalauncherContentPage *self, const char *what, GError *error)
{
  g_autofree char *message = g_strdup_printf (
      "%s: %s", what,
      error != NULL ? error->message : "something went wrong");

  if (self->banner == NULL)
    return;

  adw_banner_set_use_markup (ADW_BANNER (self->banner), FALSE);
  adw_banner_set_title (ADW_BANNER (self->banner), message);
  adw_banner_set_revealed (ADW_BANNER (self->banner), TRUE);
}

/* Remove all rows from a preferences group. */
void
take_rows_out (GList **rows, AdwPreferencesGroup *group)
{
  if (group == NULL)
    {
      g_list_free (*rows);
      *rows = NULL;
      return;
    }

  for (GList *item = *rows; item != NULL; item = item->next)
    adw_preferences_group_remove (group, item->data);

  g_list_free (*rows);
  *rows = NULL;
}

/* ---------------------------------------------------------- *
 * Installed: file rows and actions
 * ---------------------------------------------------------- */

/* Enable or disable a file by renaming it and update its row. The folder is
 * not rescanned: a rescan rebuilds every row and
 * asks Modrinth about every file a second time, and until it finished the
 * file still said it was enabled, so a fast second toggle tried to rename
 * a path that had already moved. TRUE when the file moved. */
static gboolean
toggle_file (nuvashalauncherContentPage *self, nuvashalauncherContentFile *file)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *renamed = NULL;
  g_autofree char *old_path = g_strdup (file->path);
  GtkWidget *row = NULL;

  renamed = nuvashalauncher_content_set_disabled (file->path, file->enabled, &error);

  if (renamed == NULL)
    {
       show_error (self, _("The file cannot be renamed"), error);
      return FALSE;
    }

  g_free (file->path);
  file->path = g_steal_pointer (&renamed);
  file->enabled = !file->enabled;

  /* The row the file is about follows the new path, and the table that
     finds a row by its path is told, so nothing else has to move. */
  row = g_hash_table_lookup (self->installed.rows, old_path);

  if (row != NULL)
    {
      g_hash_table_remove (self->installed.rows, old_path);
      g_hash_table_insert (self->installed.rows, g_strdup (file->path), row);
    }

  show_toast (self, file->enabled ? _("Enabled") : _("Disabled"));

  return TRUE;
}

/* The label shown for an installed file everywhere in the page.  Metadata
 * can be present without a usable project name, so the filename remains the
 * fallback in that case. */
static const char *
file_display_name (nuvashalauncherContentFile *file)
{
  if (file != NULL && file->project_name != NULL &&
      *file->project_name != '\0')
    return file->project_name;

  return file != NULL ? file->filename : NULL;
}

void
installed_filter_rows (nuvashalauncherContentPage *self)
{
  const char *query = self->installed.search_entry != NULL
                        ? gtk_editable_get_text (
                            GTK_EDITABLE (self->installed.search_entry))
                        : "";
  g_autofree char *needle = g_utf8_casefold (query, -1);

  for (GList *item = self->installed.rows_list; item != NULL; item = item->next)
    {
      GtkWidget *row = GTK_WIDGET (item->data);
      nuvashalauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
      gboolean visible = TRUE;

      if (needle != NULL && *needle != '\0')
        visible = file != NULL && file->search_key != NULL &&
                  strstr (file->search_key, needle) != NULL;

      gtk_widget_set_visible (GTK_WIDGET (row), visible);
    }
}

void
nuvashalauncher_content_page_on_installed_search_changed (GtkEditable *editable G_GNUC_UNUSED,
                             gpointer user_data)
{
  installed_filter_rows (user_data);
}

void
file_row_refresh (GtkWidget *row)
{
  nuvashalauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
  nuvashalauncherContentPage *self =
    nuvashalauncher_CONTENT_PAGE (g_object_get_data (G_OBJECT (row), "nuvashalauncher-page"));
  const char *title = NULL;

  if (file == NULL || !GTK_IS_WIDGET (row))
    return;

  {
    GtkWidget *check = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file-check");
    GtkWidget *trash = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file-trash");
    GtkWidget *sw = g_object_get_data (G_OBJECT (row), "nuvashalauncher-mod-switch");
    if (check != NULL)
      gtk_widget_set_visible (check, TRUE);
    if (trash != NULL)
      gtk_widget_set_visible (trash, TRUE);
    if (sw != NULL)
      gtk_widget_set_visible (sw, TRUE);
  }

  if (file->project_name != NULL && *file->project_name != '\0')
    title = file->project_name;

  /* The index may provide the project name before local archive metadata
   * finishes. The filename remains the fallback for unknown files. */
  adw_preferences_row_set_title (
       ADW_PREFERENCES_ROW (row),
       title != NULL ? title : file->filename);
  if (file->description != NULL && *file->description != '\0')
    row_show_description (row, file->description);

  installed_filter_rows (self);
}

static void
on_content_file_check_toggled (GtkCheckButton *check, gpointer user_data)
{
  nuvashalauncherContentPage *self = user_data;
  const char *path = g_object_get_data (G_OBJECT (check), "nuvashalauncher-file-path");

  if (path == NULL)
    return;
  if (gtk_check_button_get_active (check))
    g_hash_table_add (self->installed.selected_paths, g_strdup (path));
  else
    g_hash_table_remove (self->installed.selected_paths, path);
  content_page_update_selection (self);
}

static void
on_content_file_row_activated (AdwActionRow *row, gpointer user_data)
{
  GtkCheckButton *check = g_object_get_data (G_OBJECT (row),
                                             "nuvashalauncher-file-check");

  if (check != NULL)
    gtk_check_button_set_active (check, !gtk_check_button_get_active (check));

  (void) user_data;
}

void
content_page_update_selection (nuvashalauncherContentPage *self)
{
  guint selected = g_hash_table_size (self->installed.selected_paths);
  guint total = self->installed.files != NULL ? self->installed.files->len : 0;
  g_autofree char *text = g_strdup_printf (_("%u/%u selected"), selected, total);

  gtk_label_set_text (self->installed.selection_label, text);
  gtk_action_bar_set_revealed (self->installed.selection_footer, selected > 0);
  gtk_widget_set_visible (GTK_WIDGET (self->installed.selection_delete),
                          selected > 0);
  gtk_widget_set_sensitive (GTK_WIDGET (self->installed.selection_delete),
                            selected > 0);
}

void
content_page_toggle_edit (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherContentPage *self = user_data;
  (void) self;
}

/* The first @max_sentences sentences of @text (0 for all of it), with
 * runs of whitespace folded into single spaces. A row caps its subtitle
 * at two lines by wrapping, and an explicit blank line in the text would
 * slip a third one past that. A terminator only ends a sentence at the
 * end of the text or before a space, so "3.5" and "e.g." do not count. */
char *
description_line (const char *text, guint max_sentences)
{
  g_autoptr (GString) out = g_string_new (NULL);
  guint sentences = 0;
  gboolean space = FALSE;
  gsize length = 0;

  if (text == NULL)
    return g_strdup ("");

  length = strlen (text);

  for (gsize i = 0; i < length; i++)
    {
      char c = text[i];

      if (g_ascii_isspace (c))
        {
          space = TRUE;
          continue;
        }

      if (space && out->len > 0)
        g_string_append_c (out, ' ');

      space = FALSE;
      g_string_append_c (out, c);

      if (max_sentences != 0 &&
          (c == '.' || c == '!' || c == '?') &&
          (i + 1 == length || g_ascii_isspace (text[i + 1])))
        {
          sentences++;

          if (sentences >= max_sentences)
            break;
        }
    }

  return g_string_free (g_steal_pointer (&out), FALSE);
}

/* Build the project label and tooltip for an installed file row. */
void
row_show_description (GtkWidget *row, const char *description)
{
  g_autofree char *short_text = NULL;

  if (row == NULL || description == NULL || *description == '\0')
    return;

  short_text = description_line (description, 2);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), short_text);
}

/* The switch of a mod row: on means the mod loads, off means its file
 * carries the disabled suffix. The rename updates the row it is about, so
 * only the switch the user touched moves. When the rename fails the
 * switch stays where it was, rather than showing a state the file does
 * not have. */
static gboolean
on_mod_toggled (GtkSwitch *sw, gboolean state G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  nuvashalauncherContentFile *file = g_object_get_data (G_OBJECT (sw), "nuvashalauncher-file");

  if (file == NULL || !toggle_file (self, file))
    return TRUE;

  gtk_widget_set_tooltip_text (GTK_WIDGET (sw),
                                file->enabled ? _("Disable") : _("Enable"));

  return FALSE;
}

static void
on_deleted (GObject *source, GAsyncResult *result, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  const char *path = g_object_get_data (G_OBJECT (source), "nuvashalauncher-path");
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_content_delete_finish (G_FILE (source), result, &error))
      show_error (self, _("The file cannot be deleted"), error);
    else
    {
      g_autoptr (GPtrArray) dependent_paths =
        g_ptr_array_new_with_free_func (g_free);
      g_autoptr (nuvashalauncherSettings) settings = nuvashalauncher_settings_new ();
      nuvashalauncherContentFile *deleted = NULL;

      for (guint i = 0; i < self->installed.files->len; i++)
        {
          nuvashalauncherContentFile *candidate =
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
            nuvashalauncherContentFile *other =
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
            changed = nuvashalauncher_content_remove_file (dependent, &dependent_error);
          else
            {
              nuvashalauncherContentFile *dependent_file = NULL;
              GtkWidget *dependent_row = NULL;
              for (guint j = 0; j < self->installed.files->len; j++)
                {
                  nuvashalauncherContentFile *candidate =
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
                            toggle_file (self, dependent_file);
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
      {
        g_autofree char *directory = NULL;
        g_autofree char *filename = g_path_get_basename (path);

        g_object_get (self->instance, "directory", &directory, NULL);
        nuvashalauncher_operation_unref_when_done (nuvashalauncher_content_index_remove_async (
          directory, self->type, filename, NULL, NULL));
      }
      /* An identification request may still refer to one of the rows. Stop
         it before releasing the file it borrowed; the other rows remain in
         place and are not rebuilt. */
      g_cancellable_cancel (self->installed.scan);
      g_clear_object (&self->installed.scan);
      self->installed.scan = g_cancellable_new ();
      self->installed.identifying = FALSE;

      for (guint i = 0; path != NULL && i < self->installed.files->len; i++)
        {
          nuvashalauncherContentFile *file = g_ptr_array_index (self->installed.files, i);

          if (g_strcmp0 (file->path, path) == 0)
            {
              GtkWidget *row = GTK_WIDGET (
                g_hash_table_lookup (self->installed.rows, path));
              GList *link = g_list_find (self->installed.rows_list, row);

              if (row != NULL)
                {
                  adw_preferences_group_remove (
                      ADW_PREFERENCES_GROUP (self->installed.group), row);
                  g_hash_table_remove (self->installed.rows, path);
                }

              if (link != NULL)
                {
                  self->installed.rows_list = g_list_delete_link (
                      self->installed.rows_list, link);
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

  g_object_unref (self);
}

/* Handle the confirmation result. The path was copied onto the
 * alert, so a list that is read again while it is up cannot leave it
 * pointing at a file that is gone. */
static void
on_delete_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
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
static void
on_trash_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  nuvashalauncherContentFile *file = g_object_get_data (G_OBJECT (button), "nuvashalauncher-file");
  g_autofree char *heading = NULL;
  g_autofree char *body = NULL;
  g_autoptr (GString) dependents = g_string_new (NULL);
  g_autoptr (GPtrArray) dependent_paths =
    g_ptr_array_new_with_free_func (g_free);
  const char *title = NULL;
  AdwAlertDialog *alert = NULL;

  if (file == NULL)
    return;

  title = file_display_name (file);
  if (title == NULL || *title == '\0')
    title = file->filename;

  if (file->project_id != NULL)
    for (guint i = 0; i < self->installed.files->len; i++)
      {
        nuvashalauncherContentFile *other = g_ptr_array_index (self->installed.files, i);
        const char *other_title = NULL;

        if (other == file || other->required_dependencies == NULL)
          continue;

        for (guint j = 0; j < other->required_dependencies->len; j++)
          if (g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                         file->project_id) == 0 ||
              g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                         file->local_id) == 0)
            {
              other_title = file_display_name (other);
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
content_file_dependents (nuvashalauncherContentPage *self,
                         nuvashalauncherContentFile *file,
                         GString           *names)
{
  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

  if (file->project_id == NULL)
    return paths;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      nuvashalauncherContentFile *other = g_ptr_array_index (self->installed.files, i);
      const char *other_title;

      if (other == file || other->required_dependencies == NULL)
        continue;
      for (guint j = 0; j < other->required_dependencies->len; j++)
        if (g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                       file->project_id) == 0 ||
            g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                       file->local_id) == 0)
          {
            other_title = file_display_name (other);
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
  nuvashalauncherContentPage *self = user_data;
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
          nuvashalauncherContentFile *file = NULL;
          g_autoptr (GFile) target = NULL;
          g_autoptr (GPtrArray) dependents = NULL;

          for (guint j = 0; j < self->installed.files->len; j++)
            {
              nuvashalauncherContentFile *candidate =
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
  nuvashalauncherContentPage *self = user_data;
  GHashTableIter iter;
  gpointer key;
  GString *names = g_string_new (NULL);
  GString *dependencies = g_string_new (NULL);
  g_autofree char *heading = NULL;
  g_autofree char *body = NULL;
  g_autoptr (nuvashalauncherSettings) settings = nuvashalauncher_settings_new ();
  gboolean include_dependencies =
    nuvashalauncher_settings_get_delete_dependents (settings);

  g_hash_table_iter_init (&iter, self->installed.selected_paths);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      nuvashalauncherContentFile *file = NULL;
      for (guint i = 0; i < self->installed.files->len; i++)
        {
          nuvashalauncherContentFile *candidate =
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
      g_string_append (names, file_display_name (file));
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

GtkWidget *
file_row (nuvashalauncherContentPage *self, nuvashalauncherContentFile *file)
{
  g_return_val_if_fail (nuvashalauncher_IS_CONTENT_PAGE (self), NULL);
  g_return_val_if_fail (file != NULL, NULL);

  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-content-file-row.ui");
  AdwActionRow *row = ADW_ACTION_ROW (g_object_ref (
    gtk_builder_get_object (builder, "row")));
  GtkWidget *icon = nuvashalauncher_icon_widget_new (nuvashalauncher_content_type_icon (self->type));
  GtkCheckButton *check = GTK_CHECK_BUTTON (gtk_check_button_new ());

  /* Treat file names and paths as plain text. The tooltip contains the path;
     the subtitle contains project metadata when available. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (
    ADW_PREFERENCES_ROW (row), file_display_name (file));
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), FALSE);
  if (file->description != NULL && *file->description != '\0')
    row_show_description (GTK_WIDGET (row), file->description);

  /* Use a fixed icon square and replace the placeholder when project data
     becomes available. */
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (check), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (check), GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (check), "nuvashalauncher-file-path",
                          g_strdup (file->path), g_free);
  g_signal_connect (check, "toggled",
                    G_CALLBACK (on_content_file_check_toggled), self);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-icon", icon);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-file-check", check);

  /* Only a mod is something that can be turned off: a resource pack or a
     shader pack is loaded whenever it is present, so neither has a
     switch. */
  if (self->type == nuvashalauncher_CONTENT_MOD)
    {
      GtkSwitch *sw = GTK_SWITCH (gtk_switch_new ());

      gtk_switch_set_active (GTK_SWITCH (sw), file->enabled);
      gtk_widget_set_valign (GTK_WIDGET (sw), GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (GTK_WIDGET (sw),
                                    file->enabled ? _("Disable") : _("Enable"));
      gtk_accessible_update_property (GTK_ACCESSIBLE (sw),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       _("Enabled"), -1);
      g_object_set_data (G_OBJECT (sw), "nuvashalauncher-file", file);
      g_object_set_data (G_OBJECT (row), "nuvashalauncher-mod-switch", sw);
      g_object_set_data (
        G_OBJECT (sw), "nuvashalauncher-toggle-handler",
         GSIZE_TO_POINTER (g_signal_connect_object (
           sw, "state-set", G_CALLBACK (on_mod_toggled), self, 0)));
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (sw));
    }

  /* The selection check sits immediately to the left of the trash action. */
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (check));

  /* Every kind of file can be deleted. On a mod the button sits after
     the switch, at the right end of the row. */
  {
    GtkButton *trash = GTK_BUTTON (gtk_button_new_from_icon_name ("user-trash-symbolic"));

     nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (trash), _("Delete"));
    /* Do not let clicking delete move keyboard focus into a row that is
     * about to disappear. GTK would otherwise focus the first remaining row
     * when this button is removed, which scrolls the list to the top. */
    gtk_widget_set_focus_on_click (GTK_WIDGET (trash), FALSE);
    gtk_widget_set_valign (GTK_WIDGET (trash), GTK_ALIGN_CENTER);
     g_object_set_data (G_OBJECT (trash), "nuvashalauncher-file", file);
     g_object_set_data (G_OBJECT (row), "nuvashalauncher-file-trash", trash);
     g_signal_connect_object (trash, "clicked", G_CALLBACK (on_trash_clicked), self, 0);
     adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (trash));
  }
  /* The row carries the file it is about, so the switch does not have to
     know where the file came from. */
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-file", file);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-page", self);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_signal_connect (row, "activated",
                    G_CALLBACK (on_content_file_row_activated), self);

  file_row_refresh (GTK_WIDGET (row));

  return GTK_WIDGET (row);
}
