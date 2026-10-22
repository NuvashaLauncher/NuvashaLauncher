/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaContentPage shows installed content and the compatible Modrinth
 * catalog for one instance. The content type supplies the folder, loader
 * filter, file extensions, title, and icon.
 *
 * Async callbacks hold the page weakly: a closed page must be safe to
 * destroy while a scan, search, or version lookup is still running. */

#include "nuvasha-content-page.h"
#include "nuvasha-content-index.h"
#include "nuvasha-content-format.h"
#include "nuvasha-ui-feedback.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-i18n.h"
#include "nuvasha-icons.h"
#include "nuvasha-markdown.h"
#include "nuvasha-settings.h"

#include <glib/gstdio.h>
#include <string.h>

#include "nuvasha-content-page-private.h"
#include "nuvasha-content-files-private.h"
#include "nuvasha-operation.h"

G_DEFINE_FINAL_TYPE (NuvashaContentPage, nuvasha_content_page, GTK_TYPE_BOX)

typedef struct {
  NuvashaContentPage *page;
  GtkWidget *row;
} LocalIconLoad;

void nuvasha_content_page_run_search (NuvashaContentPage *self, gboolean append);
void update_batch_next (NuvashaContentPage *self);
void start_plan (NuvashaContentPage *self, NuvashaModrinthVersion *version,
                        const char *title, const char *replaces,
                        gboolean disabled);
void batch_next (NuvashaContentPage *self);
/* A plan that could not be carried out stops where it is; a batch it was
 * part of goes on to the next one. */
void plan_stopped (NuvashaContentPage *self);

/* ---------------------------------------------------------- *
 * Small helpers
 * ---------------------------------------------------------- */

static char *
instance_string (NuvashaInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

/* Words that are not Markdown and must read exactly as they are: an
 * error, or a description with no markup of its own. */
/* Show failures in a persistent banner. */
void
show_error (NuvashaContentPage *self, const char *what, GError *error)
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
toggle_file (NuvashaContentPage *self, NuvashaContentFile *file)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *renamed = NULL;
  g_autofree char *old_path = g_strdup (file->path);
  GtkWidget *row = NULL;

  renamed = nuvasha_content_set_disabled (file->path, file->enabled, &error);

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
file_display_name (NuvashaContentFile *file)
{
  if (file != NULL && file->project_name != NULL &&
      *file->project_name != '\0')
    return file->project_name;

  return file != NULL ? file->filename : NULL;
}

void
installed_filter_rows (NuvashaContentPage *self)
{
  const char *query = self->installed.search_entry != NULL
                        ? gtk_editable_get_text (
                            GTK_EDITABLE (self->installed.search_entry))
                        : "";
  g_autofree char *needle = g_utf8_casefold (query, -1);

  for (GList *item = self->installed.rows_list; item != NULL; item = item->next)
    {
      GtkWidget *row = GTK_WIDGET (item->data);
      NuvashaContentFile *file = g_object_get_data (G_OBJECT (row), "nuvasha-file");
      gboolean visible = TRUE;

      if (needle != NULL && *needle != '\0')
        visible = file != NULL && file->search_key != NULL &&
                  strstr (file->search_key, needle) != NULL;

      gtk_widget_set_visible (GTK_WIDGET (row), visible);
    }
}

void
nuvasha_content_page_on_installed_search_changed (GtkEditable *editable G_GNUC_UNUSED,
                             gpointer user_data)
{
  installed_filter_rows (user_data);
}

void
file_row_refresh (GtkWidget *row)
{
  NuvashaContentFile *file = g_object_get_data (G_OBJECT (row), "nuvasha-file");
  NuvashaContentPage *self =
    nuvasha_CONTENT_PAGE (g_object_get_data (G_OBJECT (row), "nuvasha-page"));
  const char *title = NULL;

  if (file == NULL || !GTK_IS_WIDGET (row))
    return;

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
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  NuvashaContentFile *file = g_object_get_data (G_OBJECT (sw), "nuvasha-file");

  if (file == NULL || !toggle_file (self, file))
    return TRUE;

  gtk_widget_set_tooltip_text (GTK_WIDGET (sw),
                                file->enabled ? _("Disable") : _("Enable"));

  return FALSE;
}

static void
on_deleted (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  const char *path = g_object_get_data (G_OBJECT (source), "nuvasha-path");
  g_autoptr (GError) error = NULL;

  if (!nuvasha_content_delete_finish (G_FILE (source), result, &error))
      show_error (self, _("The file cannot be deleted"), error);
    else
    {
      g_autoptr (GPtrArray) dependent_paths =
        g_ptr_array_new_with_free_func (g_free);
      g_autoptr (NuvashaSettings) settings = nuvasha_settings_new ();
      NuvashaContentFile *deleted = NULL;

      for (guint i = 0; i < self->installed.files->len; i++)
        {
          NuvashaContentFile *candidate =
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
            NuvashaContentFile *other =
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

          if (nuvasha_settings_get_delete_dependents (settings))
            changed = nuvasha_content_remove_file (dependent, &dependent_error);
          else
            {
              NuvashaContentFile *dependent_file = NULL;
              GtkWidget *dependent_row = NULL;
              for (guint j = 0; j < self->installed.files->len; j++)
                {
                  NuvashaContentFile *candidate =
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
                                           "nuvasha-mod-switch"));
                      if (switch_widget != NULL)
                        {
                          gulong handler = GPOINTER_TO_SIZE (
                            g_object_get_data (G_OBJECT (switch_widget),
                                               "nuvasha-toggle-handler"));

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
                          nuvasha_settings_get_delete_dependents (settings)
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
        nuvasha_operation_unref_when_done (nuvasha_content_index_remove_async (
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
          NuvashaContentFile *file = g_ptr_array_index (self->installed.files, i);

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
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  const char *path = g_object_get_data (G_OBJECT (source), "nuvasha-path");
  GPtrArray *dependent_paths =
    g_object_get_data (G_OBJECT (source), "nuvasha-dependent-paths");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
  g_autoptr (GFile) target = NULL;

  if (path == NULL || g_strcmp0 (response, "confirm") != 0)
    {
      g_object_unref (self);
      return;
    }

  target = g_file_new_for_path (path);
  g_object_set_data_full (G_OBJECT (target), "nuvasha-path",
                          g_strdup (path), g_free);
  g_object_set_data_full (G_OBJECT (target), "nuvasha-dependent-paths",
                          g_ptr_array_ref (dependent_paths),
                          (GDestroyNotify) g_ptr_array_unref);
  /* Deleting is a system call on the file and goes through GIO off the
     main loop; the ref on the page travels to on_deleted(). */
  nuvasha_content_delete_async (target, G_PRIORITY_DEFAULT, NULL, on_deleted,
                            self);
}

/* The trash button asks first, then deletes the file for good. */
static void
on_trash_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  NuvashaContentFile *file = g_object_get_data (G_OBJECT (button), "nuvasha-file");
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
        NuvashaContentFile *other = g_ptr_array_index (self->installed.files, i);
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
  g_object_set_data_full (G_OBJECT (alert), "nuvasha-path",
                          g_strdup (file->path), g_free);
  g_object_set_data_full (G_OBJECT (alert), "nuvasha-dependent-paths",
                          g_steal_pointer (&dependent_paths),
                          (GDestroyNotify) g_ptr_array_unref);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_delete_chosen,
                           g_object_ref (self));
}

GtkWidget *
file_row (NuvashaContentPage *self, NuvashaContentFile *file)
{
  g_return_val_if_fail (nuvasha_IS_CONTENT_PAGE (self), NULL);
  g_return_val_if_fail (file != NULL, NULL);

  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkWidget *icon = nuvasha_icon_widget_new (nuvasha_content_type_icon (self->type));

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
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
  g_object_set_data (G_OBJECT (row), "nuvasha-icon", icon);

  /* Only a mod is something that can be turned off: a resource pack or a
     shader pack is loaded whenever it is present, so neither has a
     switch. */
  if (self->type == nuvasha_CONTENT_MOD)
    {
      GtkSwitch *sw = GTK_SWITCH (gtk_switch_new ());

      gtk_switch_set_active (GTK_SWITCH (sw), file->enabled);
      gtk_widget_set_valign (GTK_WIDGET (sw), GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (GTK_WIDGET (sw),
                                    file->enabled ? _("Disable") : _("Enable"));
      gtk_accessible_update_property (GTK_ACCESSIBLE (sw),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       _("Enabled"), -1);
      g_object_set_data (G_OBJECT (sw), "nuvasha-file", file);
      g_object_set_data (G_OBJECT (row), "nuvasha-mod-switch", sw);
      g_object_set_data (
        G_OBJECT (sw), "nuvasha-toggle-handler",
        GSIZE_TO_POINTER (g_signal_connect (
          sw, "state-set", G_CALLBACK (on_mod_toggled), self)));
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (sw));
    }

  /* Every kind of file can be deleted. On a mod the button sits after
     the switch, at the right end of the row. */
  {
    GtkButton *trash = GTK_BUTTON (gtk_button_new_from_icon_name ("user-trash-symbolic"));

    gtk_widget_add_css_class (GTK_WIDGET (trash), "flat");
    /* Do not let clicking delete move keyboard focus into a row that is
     * about to disappear. GTK would otherwise focus the first remaining row
     * when this button is removed, which scrolls the list to the top. */
    gtk_widget_set_focus_on_click (GTK_WIDGET (trash), FALSE);
    gtk_widget_set_valign (GTK_WIDGET (trash), GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text (GTK_WIDGET (trash), _("Delete"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (trash),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                     _("Delete"), -1);
    g_object_set_data (G_OBJECT (trash), "nuvasha-file", file);
    g_signal_connect_object (trash, "clicked", G_CALLBACK (on_trash_clicked), self, 0);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (trash));
  }

  /* The row carries the file it is about, so the switch does not have to
     know where the file came from. */
  g_object_set_data (G_OBJECT (row), "nuvasha-file", file);
  g_object_set_data (G_OBJECT (row), "nuvasha-page", self);

  file_row_refresh (GTK_WIDGET (row));

  return GTK_WIDGET (row);
}

static void
local_icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  LocalIconLoad *load = user_data;
  NuvashaContentFile *file = g_object_get_data (G_OBJECT (load->row), "nuvasha-file");
  g_autoptr (GError) error = NULL;
  g_autofree char *path =
    nuvasha_content_load_local_icon_finish (result, &error);

  g_object_set_data (G_OBJECT (load->row), "nuvasha-local-icon-loading", NULL);

  if (path != NULL && file != NULL)
    {
      g_free (file->local_icon_path);
      file->local_icon_path = g_steal_pointer (&path);

      if (load->page->installed.scroller != NULL)
        {
          GtkWidget *icon = GTK_WIDGET (
            g_object_get_data (G_OBJECT (load->row), "nuvasha-icon"));
          if (icon != NULL)
            nuvasha_icon_widget_set_file (icon, file->local_icon_path);
        }
      persist_identified_file (load->page, file);
    }

  g_object_unref (load->row);
  g_object_unref (load->page);
  g_free (load);
}

static void
load_row_icon (NuvashaContentPage *self, GtkWidget *row)
{
  NuvashaContentFile *file = g_object_get_data (G_OBJECT (row), "nuvasha-file");
  GtkWidget *icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (row), "nuvasha-icon"));
  GtkWidget *scroller = GTK_WIDGET (self->installed.scroller);
  graphene_rect_t bounds;

  if (file == NULL || icon == NULL ||
      (file->local_icon_path == NULL && file->local_icon_member == NULL) ||
      g_object_get_data (G_OBJECT (row), "nuvasha-icon-loaded") != NULL ||
      !gtk_widget_compute_bounds (row, scroller, &bounds))
    return;

  if (bounds.origin.y + bounds.size.height < 0 ||
      bounds.origin.y > gtk_widget_get_height (scroller))
    return;

  if (file->local_icon_path != NULL &&
      !g_file_test (file->local_icon_path, G_FILE_TEST_IS_REGULAR))
    g_clear_pointer (&file->local_icon_path, g_free);

  if (file->local_icon_path == NULL &&
      g_object_get_data (G_OBJECT (row), "nuvasha-local-icon-loading") == NULL)
    {
      LocalIconLoad *load = g_new0 (LocalIconLoad, 1);

      load->page = g_object_ref (self);
      load->row = g_object_ref (row);
      g_object_set_data (G_OBJECT (row), "nuvasha-local-icon-loading",
                         GINT_TO_POINTER (1));

      nuvasha_operation_release (
        nuvasha_content_load_local_icon_async (file, local_icon_ready, load),
        self->installed.scan);
    }

  if (file->local_icon_path != NULL &&
      nuvasha_icon_widget_set_file (icon, file->local_icon_path))
    g_object_set_data (G_OBJECT (row), "nuvasha-icon-loaded", GINT_TO_POINTER (1));
}

void
load_visible_icons (NuvashaContentPage *self)
{
  for (GList *item = self->installed.rows_list; item != NULL; item = item->next)
    load_row_icon (self, item->data);
}

void
nuvasha_content_page_on_installed_scroll_changed (
  GtkAdjustment *adjustment G_GNUC_UNUSED, gpointer user_data)
{
  load_visible_icons (user_data);
}





static void
open_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_default_for_uri_finish (result, &error))
    g_warning ("the folder does not open: %s", error->message);

  g_free (user_data);
}

void
nuvasha_content_page_on_open_folder_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  g_autofree char *uri = NULL;

  /* The folder may not exist yet on a fresh instance: asking the desktop
     to open a path that is not there only confuses it. Create it first,
     empty, ready for files. */
  g_autoptr (GError) create_error = NULL;
  if (!nuvasha_content_ensure_folder (self->folder, &create_error))
    {
      g_autofree char *message =
        g_strdup_printf (_("Cannot create \"%s\": %s"), self->folder,
                          create_error->message);

      show_toast (self, message);
      return;
    }

  uri = g_filename_to_uri (self->folder, NULL, NULL);

  if (uri == NULL)
    return;

  /* GIO hands the folder to the desktop the way a double click in a file
   * manager does: nothing goes through a shell. */
  g_app_info_launch_default_for_uri_async (uri, NULL, NULL, open_finished,
                                           g_strdup (uri));
}

NuvashaContentPage *
nuvasha_content_page_new (NuvashaInstance *instance, NuvashaContentType type)
{
  NuvashaContentPage *self = NULL;
  g_autofree char *directory = NULL;
  const char *loader = NULL;
  NuvashaLoader value = nuvasha_LOADER_VANILLA;

  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), NULL);

  self = g_object_new (nuvasha_TYPE_CONTENT_PAGE, NULL);
  self->instance = g_object_ref (instance);
  self->type = type;
  self->minecraft_version = instance_string (instance, "minecraft-version");
  self->browse.model = nuvasha_content_browser_model_new ();
  self->installed.scan = g_cancellable_new ();

  g_object_get (instance, "directory", &directory, NULL);

  if (directory != NULL)
    self->folder = nuvasha_content_folder_path (directory, type);

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* Mods require a non-vanilla loader. */
  g_object_get (instance, "loader", &value, NULL);

  if (nuvasha_content_type_needs_loader (type))
    {
      if (value == nuvasha_LOADER_VANILLA)
        {
          AdwStatusPage *status = ADW_STATUS_PAGE (adw_status_page_new ());

          adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                         nuvasha_content_type_icon (type));
          adw_status_page_set_title (ADW_STATUS_PAGE (status),
                                      _("No mod loader"));
          adw_status_page_set_description (
              ADW_STATUS_PAGE (status),
              _("Mods need a mod loader. Pick Fabric, Quilt, Forge or NeoForge "
              "in the settings of this instance first"));
          gtk_box_append (GTK_BOX (self), GTK_WIDGET (status));

          return self;
        }
    }

  /* Modrinth expects lowercase loader names. */
  g_object_get (instance, "loader", &value, NULL);
  loader = nuvasha_loader_to_string (value);
  self->loader = loader != NULL ? g_strdup (loader) : NULL;

  self->project.details_ready = FALSE;
  self->project.versions_ready = FALSE;
  self->project.details = NULL;
  self->project.details_versions = NULL;
  self->project.details_problem = NULL;
  self->project.versions_problem = NULL;
  self->project.current_project_title = NULL;
  self->project.current_project_id = NULL;
  self->project.current_project_downloads = 0;
  self->project.details_author = NULL;
  self->project.details_body = NULL;
  self->project.details_dropdown = NULL;
  self->project.details_download = NULL;
  self->installed.identifying = FALSE;
  self->project.details_check = NULL;
  self->project.details_page = NULL;
  self->project.split = NULL;
  self->install_model->batch = NULL;
  self->install_model->picking = FALSE;
  self->install_model->pick_index = 0;
  self->pick_button = NULL;
  self->pick_badge = NULL;

  self->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);

  nuvasha_content_page_build_installed (self);
  nuvasha_content_page_build_browse (self);

  g_signal_connect_object (self->stack, "notify::visible-child-name",
                           G_CALLBACK (nuvasha_content_page_on_stack_switched),
                           self, 0);

  nuvasha_content_page_build_bar (self);
  nuvasha_content_page_move_actions_into_bar (self, FALSE);

  return self;
}

static void
nuvasha_content_page_dispose (GObject *object)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (object);

  /* The widget tree may be freed while the page is still answering a
   * request of its own: nothing of it is touched afterwards. */
  self->toasts = NULL;
  self->banner = NULL;
  self->stack = NULL;
  self->project.split = NULL;
  self->bar = NULL;
  self->bar_spacer = NULL;
  self->switcher = NULL;
  self->switcher_installed = NULL;
  self->switcher_browse = NULL;
  /* Release the action rows retained by the page. */
  g_clear_object (&self->installed.actions);
  g_clear_object (&self->browse_actions);
  self->installed.group = NULL;
  self->installed.scroller = NULL;
  self->installed.status = NULL;
  self->installed.loading = NULL;
  self->installed.update_content = NULL;
  self->installed.update_spinner = NULL;
  self->installed.search_bar = NULL;
  self->installed.search_entry = NULL;
  self->installed.update_all = NULL;
  self->browse.results_group = NULL;
  self->browse.results_scroller = NULL;
  self->browse.results_status = NULL;
  self->browse.search_entry = NULL;
  self->browse.search_bar = NULL;
  self->browse.filter_box = NULL;
  self->browse.loading_more = NULL;
  self->browse.retry_row = NULL;
  self->browse.retry_label = NULL;
  self->browse.spinner = NULL;
  self->pick_button = NULL;
  self->pick_badge = NULL;

  /* A batch that was still being installed is not applied any more: the
     plans and fetches hold the page weakly and stop. */
  g_clear_pointer (&self->install_model->batch, g_ptr_array_unref);
  self->install_model->picking = FALSE;
  self->install_model->pick_index = 0;

  if (self->browse.model->debounce_source != 0)
    {
      /* Through the model, which is where this source is registered: a
         scope left holding the id would take down whatever source is
         given the same id next. */
      nuvasha_operation_remove (self->browse.model->scope,
                                    self->browse.model->debounce_source);
      self->browse.model->debounce_source = 0;
    }

  g_cancellable_cancel (self->installed.scan);
  g_clear_object (&self->installed.scan);

  /* The page of a project and the widgets in it are owned by the
     page; the fields only watch them, so they are dropped and not
     unrefed. */
  watch_widget (&self->project.details_body, NULL);
  watch_widget (&self->project.details_check, NULL);
  watch_widget (&self->project.details_dropdown, NULL);
  watch_widget (&self->project.details_download, NULL);
  watch_widget (&self->project.details_page, NULL);

  /* A batch of updates that was still being applied is not applied any
     more: the plans and fetches hold the page weakly and stop. */
  self->install_model->updating = FALSE;

  G_OBJECT_CLASS (nuvasha_content_page_parent_class)->dispose (object);
}

static void
nuvasha_content_page_finalize (GObject *object)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (object);

  g_clear_object (&self->instance);
  g_clear_pointer (&self->installed.files, g_ptr_array_unref);
  g_clear_pointer (&self->installed.rows, g_hash_table_unref);
  nuvasha_content_browser_model_free (self->browse.model);
  self->browse.model = NULL;
  g_list_free (self->installed.rows_list);
  g_list_free (self->browse.result_rows);
  nuvasha_content_install_model_free (self->install_model);
  self->install_model = NULL;
  g_free (self->minecraft_version);
  g_free (self->loader);
  g_free (self->folder);
  g_free (self->project.current_project_title);
  g_free (self->project.current_project_id);
  g_free (self->project.details_author);
  g_clear_pointer (&self->project.details, nuvasha_modrinth_details_free);
  g_clear_pointer (&self->project.details_versions, g_ptr_array_unref);
  g_free (self->project.details_problem);
  g_free (self->project.versions_problem);

  G_OBJECT_CLASS (nuvasha_content_page_parent_class)->finalize (object);
}

static void
nuvasha_content_page_class_init (NuvashaContentPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_content_page_dispose;
  object_class->finalize = nuvasha_content_page_finalize;
}

static void
nuvasha_content_page_init (NuvashaContentPage *self)
{
  /* Keep the selected-file list and required loader state. Everything else
     is built in nuvasha_content_page_new(), which knows the instance and the
     kind of content. */
  self->install_model =
    nuvasha_content_install_model_new ((GDestroyNotify) pick_free);
}
