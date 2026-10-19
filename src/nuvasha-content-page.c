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

static void persist_identified_file (NuvashaContentPage *self,
                                     NuvashaContentFile *file);

/* One file with an available update. */
typedef struct {
  NuvashaModrinthVersion *version; /* the newer version, owned */
  char *title;
  char *path;
  gboolean disabled;
} UpdateItem;

/* State for an update check. */
typedef struct {
  NuvashaContentPage *page; /* weakly held */
  guint index;
  GPtrArray *items; /* UpdateItem* */
} UpdateRun;

static void
update_run_free (UpdateRun *run)
{
  if (run == NULL)
    return;

  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page),
                                  (gpointer *) &run->page);
  g_clear_pointer (&run->items, g_ptr_array_unref);
  g_free (run);
}

void nuvasha_content_page_run_search (NuvashaContentPage *self, gboolean append);
static void update_check_next (UpdateRun *run);
void update_batch_next (NuvashaContentPage *self);
void start_plan (NuvashaContentPage *self, NuvashaModrinthVersion *version,
                        const char *title, const char *replaces,
                        gboolean disabled);
void batch_next (NuvashaContentPage *self);
/* A plan that could not be carried out stops where it is; a batch it was
 * part of goes on to the next one. */
void plan_stopped (NuvashaContentPage *self);
static void metadata_ready (GObject *source, GAsyncResult *result,
                            gpointer user_data);
static void load_visible_icons (NuvashaContentPage *self);

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

static void
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
  adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)), short_text);
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
                               file->enabled ? "Disable" : "Enable");

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
                            GTK_WIDGET (switch_widget), "Enable");
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

static GtkWidget *
file_row (NuvashaContentPage *self, NuvashaContentFile *file)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkWidget *icon = nuvasha_icon_widget_new (nuvasha_content_type_icon (self->type));

  /* Treat file names and paths as plain text. The tooltip contains the path;
     the subtitle contains project metadata when available. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), FALSE);
  adw_preferences_row_set_title (
    ADW_PREFERENCES_ROW (GTK_WIDGET (row)), file_display_name (file));
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (GTK_WIDGET (row)), 2);
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (GTK_WIDGET (row)), FALSE);
  if (file->description != NULL && *file->description != '\0')
    row_show_description (GTK_WIDGET (row), file->description);

  /* Use a fixed icon square and replace the placeholder when project data
     becomes available. */
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (GTK_WIDGET (row)), icon);
  g_object_set_data (G_OBJECT (row), "nuvasha-icon", icon);

  /* Only a mod is something that can be turned off: a resource pack or a
     shader pack is loaded whenever it is present, so neither has a
     switch. */
  if (self->type == nuvasha_CONTENT_MOD)
    {
      GtkSwitch *sw = GTK_SWITCH (gtk_switch_new ());

      gtk_switch_set_active (GTK_SWITCH (GTK_WIDGET (sw)), file->enabled);
      gtk_widget_set_valign (GTK_WIDGET (sw), GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (GTK_WIDGET (sw),
                                   file->enabled ? "Disable" : "Enable");
      gtk_accessible_update_property (GTK_ACCESSIBLE (GTK_WIDGET (sw)),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Enabled", -1);
      g_object_set_data (G_OBJECT (sw), "nuvasha-file", file);
      g_object_set_data (G_OBJECT (row), "nuvasha-mod-switch", sw);
      g_object_set_data (
        G_OBJECT (sw), "nuvasha-toggle-handler",
        GSIZE_TO_POINTER (g_signal_connect (
          sw, "state-set", G_CALLBACK (on_mod_toggled), self)));
      adw_action_row_add_suffix (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (sw));
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
    gtk_accessible_update_property (GTK_ACCESSIBLE (GTK_WIDGET (trash)),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Delete", -1);
    g_object_set_data (G_OBJECT (trash), "nuvasha-file", file);
    g_signal_connect (trash, "clicked", G_CALLBACK (on_trash_clicked), self);
    adw_action_row_add_suffix (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (trash));
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

static void
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

/* Ask Modrinth several hashes at once, while keeping the request burst
 * bounded for large content folders. */
#define IDENTIFY_CONCURRENCY 8

typedef struct _IdentifyRun IdentifyRun;
typedef struct {
  IdentifyRun *run;
  NuvashaContentFile *file;
  GtkWidget *row;

  /* The operation that answers for this file, released with the request. */
  NuvashaTaskScope *operation;
} IdentifyRequest;

typedef struct {
  NuvashaContentPage *page; /* weakly held */
  char *project_id;
  NuvashaTaskScope *operation;
} ProjectTitleRequest;

struct _IdentifyRun {
  NuvashaContentPage *page; /* weakly held */
  GCancellable *cancellable;
  guint next_index;
  guint in_flight;
  gboolean finished;
};

static void identify_start (IdentifyRun *run);

static void
identify_run_maybe_free (IdentifyRun *run)
{
  if (!run->finished || run->in_flight != 0)
    return;
  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page),
                                  (gpointer *) &run->page);

  g_clear_object (&run->cancellable);
  g_free (run);
}

static void
identify_complete (IdentifyRun *run)
{
  if (!run->finished)
    {
      run->finished = TRUE;
      if (run->page != NULL)
        {
          run->page->installed.identifying = FALSE;
          if (run->page->project.details_versions != NULL &&
              run->page->project.details_download != NULL)
            {
              gboolean have_versions =
                run->page->project.details_versions->len > 0;
              gtk_widget_set_sensitive (
                GTK_WIDGET (run->page->project.details_download), have_versions);
              gtk_widget_set_sensitive (
                GTK_WIDGET (run->page->project.details_check), have_versions);
            }
        }
    }
  identify_run_maybe_free (run);
}

static void
identify_request_free (IdentifyRequest *request)
{
  IdentifyRun *run = request->run;

  if (request->operation != NULL)
    nuvasha_operation_unref (request->operation);

  g_free (request);
  run->in_flight--;
  identify_run_maybe_free (run);
}

static void
persist_identified_file (NuvashaContentPage *self, NuvashaContentFile *file)
{
  g_autofree char *directory = NULL;
  g_autofree char *filename = g_path_get_basename (file->path);
  NuvashaContentIndexEntry entry = { 0 };

  g_object_get (self->instance, "directory", &directory, NULL);
  entry.filename = filename;
  entry.size = file->size;
  entry.mtime_usec = file->mtime_usec;
  entry.hash = file->sha1;
  entry.hash512 = file->sha512;
  entry.download_url = file->download_url;
  entry.env_client = "required";
  entry.env_server = self->type == nuvasha_CONTENT_MOD ? "required"
                                                    : "unsupported";
  entry.provider = "Modrinth";
  entry.project_id = file->project_id;
  entry.project_name = file->project_name;
  entry.icon_url = file->icon_url;
  entry.metadata_checked = file->metadata_checked;
  entry.local_icon_member = file->local_icon_member;
  entry.local_icon_path = file->local_icon_path;
  entry.version_id = file->version_id;
  entry.version_name = file->version_name;
  entry.required_dependencies = file->required_dependencies;
  entry.optional_dependencies = file->optional_dependencies;
  entry.incompatible_dependencies = file->incompatible_dependencies;
  nuvasha_operation_release (
    nuvasha_content_index_update_async (directory, self->type, &entry, NULL, NULL),
    self->installed.scan);
}

static void
project_title_request_free (ProjectTitleRequest *request)
{
  if (request->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (request->page),
                                  (gpointer *) &request->page);
  if (request->operation != NULL)
    nuvasha_operation_unref (request->operation);
  g_free (request->project_id);
  g_free (request);
}

static void
on_project_title_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                         gpointer user_data)
{
  ProjectTitleRequest *request = user_data;
  NuvashaContentPage *self = request->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModrinthDetails) details =
    nuvasha_modrinth_details_finish (result, &error);

  if (self != NULL && details != NULL && details->title != NULL &&
      *details->title != '\0')
    {
      if (self->browse.model == NULL || self->installed.files == NULL)
        {
          project_title_request_free (request);
          return;
        }

      g_hash_table_replace (self->browse.model->project_titles,
                            g_strdup (request->project_id),
                            g_strdup (details->title));

      for (guint i = 0; i < self->installed.files->len; i++)
        {
          NuvashaContentFile *file =
            g_ptr_array_index (self->installed.files, i);

          if (g_strcmp0 (file->project_id, request->project_id) != 0)
            continue;
          if (file->project_name != NULL &&
              *file->project_name != '\0' &&
              g_strcmp0 (file->project_name, file->project_id) != 0)
            continue;

          g_free (file->project_name);
          file->project_name = g_strdup (details->title);
          persist_identified_file (self, file);

          GtkWidget *row = self->installed.rows != NULL
                             ? GTK_WIDGET (g_hash_table_lookup (
                                 self->installed.rows, file->path))
                             : NULL;
          if (row != NULL)
            file_row_refresh (row);
        }
    }

  project_title_request_free (request);
}

static void
request_project_title (NuvashaContentPage *self, const char *project_id)
{
  ProjectTitleRequest *request = NULL;

  if (self == NULL || project_id == NULL || *project_id == '\0')
    return;

  request = g_new0 (ProjectTitleRequest, 1);
  request->page = self;
  request->project_id = g_strdup (project_id);
  g_object_add_weak_pointer (G_OBJECT (self),
                             (gpointer *) &request->page);
  request->operation = nuvasha_modrinth_details_async (
    project_id, on_project_title_ready, request);
  if (request->operation == NULL)
    {
      project_title_request_free (request);
      return;
    }
}

static void
add_remote_dependency (GPtrArray **destination, const char *value)
{
  if (value == NULL)
    return;
  if (*destination == NULL)
    *destination = g_ptr_array_new_with_free_func (g_free);
  for (guint j = 0; j < (*destination)->len; j++)
    if (g_strcmp0 (value, g_ptr_array_index (*destination, j)) == 0)
      return;
  g_ptr_array_add (*destination, g_strdup (value));
}

static void
identify_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  IdentifyRequest *request = user_data;
  IdentifyRun *run = request->run;
  NuvashaContentPage *self = run->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModrinthVersion) version = NULL;
  NuvashaContentFile *file = request->file;
  GtkWidget *row = request->row;

  version = nuvasha_modrinth_version_by_hash_finish (result, &error);

  /* A new folder is being read now: the chain's cancellable is cancelled,
   * so the chain of the old answer stops. */
  if (run->cancellable != NULL &&
      g_cancellable_is_cancelled (run->cancellable))
    {
      run->finished = TRUE;
      identify_request_free (request);
      return;
    }

  if (self == NULL || file == NULL || row == NULL)
    {
      identify_complete (run);
      identify_request_free (request);
      return;
    }

  if (version != NULL)
    {
      file->project_id = g_strdup (version->project_id);
      {
        const char *known_name = g_hash_table_lookup (self->browse.model->project_titles,
                                                      version->project_id);
        if (!file->local_metadata || file->project_name == NULL ||
            *file->project_name == '\0' ||
            g_strcmp0 (file->project_name, version->project_id) == 0)
          {
            g_free (file->project_name);
            file->project_name = g_strdup (known_name);
          }
        if (known_name == NULL)
          request_project_title (self, version->project_id);
      }
      file->version_id = g_strdup (version->version_id);
      file->version_name = g_strdup (version->name);
      file->version_number = g_strdup (version->version_number);
      {
        NuvashaModrinthFile *primary =
          nuvasha_modrinth_version_primary_file (version);

        g_free (file->sha512);
        g_free (file->download_url);
        file->sha512 = primary != NULL ? g_strdup (primary->sha512) : NULL;
        file->download_url = primary != NULL ? g_strdup (primary->url) : NULL;
      }
      for (guint i = 0; version->dependencies != NULL &&
                       i < version->dependencies->len; i++)
        {
          NuvashaModrinthDependency *dependency =
            g_ptr_array_index (version->dependencies, i);

          if (dependency->project_id == NULL)
            continue;
          if (dependency->type == nuvasha_DEPENDENCY_REQUIRED)
            add_remote_dependency (&file->required_dependencies,
                                   dependency->project_id);
          else if (dependency->type == nuvasha_DEPENDENCY_OPTIONAL)
            add_remote_dependency (&file->optional_dependencies,
                                   dependency->project_id);
          else if (dependency->type == nuvasha_DEPENDENCY_INCOMPATIBLE)
            add_remote_dependency (&file->incompatible_dependencies,
                                   dependency->project_id);
        }

      /* Persist the hash lookup immediately. Project details and the icon
       * are a second request; a failure there must not lose the metadata we
       * already obtained from the version-by-hash response. */
      persist_identified_file (self, file);

      if (file->project_id != NULL && *file->project_id != '\0')
        g_hash_table_add (self->browse.model->projects, g_strdup (file->project_id));

      file_row_refresh (row);

      /* The browser's icon of the project the file belongs to, which the
         row carries from here on. */
    }

  /* Modrinth does not know this file, or the network is not there: it
   * keeps its own name and the page works all the same. */
  g_clear_error (&error);

  identify_request_free (request);
  identify_start (run);
}

static void
identify_start (IdentifyRun *run)
{
  NuvashaContentPage *self = run->page;

  if (self == NULL)
    {
      identify_complete (run);
      return;
    }

  while (run->next_index < self->installed.files->len &&
         run->in_flight < IDENTIFY_CONCURRENCY)
    {
      NuvashaContentFile *file =
        g_ptr_array_index (self->installed.files, run->next_index++);
      g_autofree char *key = NULL;
      GtkWidget *row = NULL;

      key = g_strdup (file->path);
      row = g_hash_table_lookup (self->installed.rows, key);

      /* A hash-matched index entry already identifies this file. The row is
       * ready from disk and reopening an instance must not ask Modrinth just
       * to rediscover metadata already stored in the index. */
      if (row == NULL || file->sha1 == NULL)
        continue;

      IdentifyRequest *request = g_new0 (IdentifyRequest, 1);
      request->run = run;
      request->file = file;
      request->row = row;
      run->in_flight++;
      if (file->version_id != NULL && *file->version_id != '\0')
        request->operation =
          nuvasha_modrinth_version_by_id_async (file->version_id, identify_ready,
                                            request);
      else
        request->operation =
          nuvasha_modrinth_version_by_hash_async (file->sha1, identify_ready,
                                              request);

      if (run->cancellable != NULL)
        nuvasha_operation_follow (request->operation,
                                           run->cancellable);
    }

  if (run->next_index >= self->installed.files->len && run->in_flight == 0)
    identify_complete (run);
}

static void
scan_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  PageWatch *watch = user_data;
  NuvashaContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) files = NULL;

  /* The answer is taken even when nobody is left to show it: the result
     belongs to whoever asked, and it is freed by being handed over. */
  files = nuvasha_content_scan_finish (result, &error);

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  if (files == NULL)
    {
      gtk_widget_set_visible (GTK_WIDGET (self->installed.loading), FALSE);
      gtk_widget_set_visible (GTK_WIDGET (self->installed.status), TRUE);
      if (error != NULL && !g_error_matches (error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED))
         show_error (self, _("The folder cannot be read"), error);

      page_watch_free (watch);
      return;
    }

  gtk_widget_set_visible (GTK_WIDGET (self->installed.loading), FALSE);

  /* The rows that pointed into the old list are gone, so the list they
   * pointed into can go too. */
  take_rows_out (&self->installed.rows_list,
                 ADW_PREFERENCES_GROUP (self->installed.group));
  g_clear_pointer (&self->installed.files, g_ptr_array_unref);
  g_clear_pointer (&self->installed.rows, g_hash_table_unref);
  self->installed.files = g_steal_pointer (&files);
  self->installed.rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      NuvashaContentFile *file = g_ptr_array_index (self->installed.files, i);

      if (file->project_id != NULL && file->project_name != NULL &&
          *file->project_name != '\0')
        g_hash_table_replace (self->browse.model->project_titles,
                              g_strdup (file->project_id),
                              g_strdup (file->project_name));
      GtkWidget *row = file_row (self, file);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->installed.group),
                                  row);
      self->installed.rows_list = g_list_append (self->installed.rows_list, row);
      g_hash_table_insert (self->installed.rows, g_strdup (file->path), row);
    }

  installed_filter_rows (self);

  {
    gboolean empty = self->installed.files->len == 0;

    gtk_widget_set_visible (GTK_WIDGET (self->installed.status), empty);
    gtk_widget_set_visible (GTK_WIDGET (self->installed.scroller), !empty);
    gtk_widget_set_sensitive (GTK_WIDGET (self->installed.update_all), !empty);
    if (!empty)
      load_visible_icons (self);
  }

  {
    IdentifyRun *run = g_new0 (IdentifyRun, 1);

    self->installed.identifying = TRUE;
    run->page = self;
    g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
    run->cancellable = g_object_ref (self->installed.scan);
    identify_start (run);
  }

  nuvasha_operation_release (
    nuvasha_content_metadata_async (self->installed.files, self->type,
                                metadata_ready, g_object_ref (self)),
    self->installed.scan);

  page_watch_free (watch);
}

static void
metadata_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  g_autoptr (GError) error = NULL;

  if (nuvasha_content_metadata_finish (result, &error))
    {
      for (guint i = 0; self->installed.files != NULL &&
                        i < self->installed.files->len; i++)
        {
           NuvashaContentFile *file =
             g_ptr_array_index (self->installed.files, i);
          GtkWidget *row = GTK_WIDGET (
            g_hash_table_lookup (self->installed.rows, file->path));

           if (row != NULL)
             file_row_refresh (row);

           if (file->metadata_dirty)
             {
               persist_identified_file (self, file);
               file->metadata_dirty = FALSE;
             }
        }
      installed_filter_rows (self);
    }

  g_object_unref (self);
}

void
reload_installed (NuvashaContentPage *self)
{
  g_autofree char *directory = NULL;
  GCancellable *previous = self->installed.scan;

  if (self->installed.scan == NULL || self->installed.group == NULL)
    return;

  self->installed.loaded = TRUE;

  g_object_get (self->instance, "directory", &directory, NULL);

  if (directory == NULL)
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->installed.status), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->installed.scroller), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->installed.loading), TRUE);

  /* Cancel scans for the previous directory contents. */
  self->installed.scan = g_cancellable_new ();
  g_cancellable_cancel (previous);
  g_clear_object (&previous);

  {
    PageWatch *watch = page_watch_new (self);

    watch->operation = nuvasha_content_scan_async (directory, self->type,
                                               scan_ready, watch);
    nuvasha_operation_follow (watch->operation,
                                       self->installed.scan);
  }
}

  /* Update All: collect updates, confirm once, then install them. */

static void
update_item_free (UpdateItem *item)
{
  if (item == NULL)
    return;

  nuvasha_modrinth_version_free (item->version);
  g_free (item->title);
  g_free (item->path);
  g_free (item);
}

static void
update_button_busy (NuvashaContentPage *self, gboolean busy)
{
  if (self->installed.update_content == NULL)
    return;
  gtk_stack_set_visible_child_name (self->installed.update_content,
                                    busy ? "spinner" : "icon");
  gtk_widget_set_sensitive (
    GTK_WIDGET (self->installed.update_all),
    self->installed.files != NULL && self->installed.files->len > 0);
  gtk_widget_set_can_target (GTK_WIDGET (self->installed.update_all), !busy);
  gtk_widget_set_focusable (GTK_WIDGET (self->installed.update_all), !busy);
}

/* Starts the next one of a confirmed batch. When the last one is in, the
 * list is read again and the batch is over. */
void
update_batch_next (NuvashaContentPage *self)
{
  UpdateItem *item = NULL;

  if (self->install_model->updates == NULL || self->install_model->update_index >= self->install_model->updates->len)
    {
      guint done = self->install_model->updates != NULL ? self->install_model->updates->len : 0;

      g_clear_pointer (&self->install_model->updates, g_ptr_array_unref);
      self->install_model->update_index = 0;
      self->install_model->updating = FALSE;
      update_button_busy (self, FALSE);

      if (done > 0)
        {
          g_autofree char *message = g_strdup_printf (
            ngettext ("Updated %u item", "Updated %u items", done), done);

          show_toast (self, message);
          reload_installed (self);
        }

      return;
    }

  item = g_ptr_array_index (self->install_model->updates, self->install_model->update_index);
  self->install_model->update_index++;

  /* The batch was confirmed once, as a whole: this plan is not asked for
     again, it is simply carried out. */
  update_button_busy (self, TRUE);
  start_plan (self, nuvasha_modrinth_version_copy (item->version), item->title,
              item->path, item->disabled);
}

void
plan_stopped (NuvashaContentPage *self)
{
  if (self->install_model->updating)
    update_batch_next (self);
  else if (self->install_model->picking)
    batch_next (self);
  else
    set_busy (self, FALSE);
}

/* Handle the update confirmation result. */
static void
on_update_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  UpdateRun *run = g_object_get_data (G_OBJECT (source), "nuvasha-update-run");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (run == NULL || self == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (g_strcmp0 (response, "update") != 0)
    {
      g_object_set_data (G_OBJECT (self), "nuvasha-update-run", NULL);
      update_run_free (run);
      g_object_unref (self);
      return;
    }

  /* The whole list was confirmed: install each one. */
  self->install_model->updates = g_steal_pointer (&run->items);
  self->install_model->update_index = 0;
  self->install_model->updating = TRUE;
  g_object_set_data (G_OBJECT (self), "nuvasha-update-run", NULL);
  update_run_free (run);
  update_batch_next (self);

  g_object_unref (self);
}

/* Finish the update check and show the confirmation dialog or an up-to-date
 * message. */
static void
update_check_done (NuvashaContentPage *self, UpdateRun *run)
{
  update_button_busy (self, FALSE);
  g_object_set_data (G_OBJECT (self), "nuvasha-update-run", NULL);

  if (run->items->len == 0)
    {
      show_toast (self, _("Everything is up to date"));
      update_run_free (run);
      return;
    }

  {
    const char *what = self->type == nuvasha_CONTENT_MOD
                         ? "mods"
                         : self->type == nuvasha_CONTENT_RESOURCE_PACK
                             ? "resource packs"
                             : "shader packs";
    g_autofree char *heading =
      g_strdup_printf (_("Update the following %s"), what);
    g_autoptr (GString) body = g_string_new (NULL);
    AdwAlertDialog *alert = NULL;

    for (guint i = 0; i < run->items->len; i++)
      {
        UpdateItem *item = g_ptr_array_index (run->items, i);

        if (i > 0)
          g_string_append_c (body, '\n');

        g_string_append (body, item->title);
      }

    alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body->str));
    adw_alert_dialog_set_heading_use_markup (alert, FALSE);
    adw_alert_dialog_set_body_use_markup (alert, FALSE);
    adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
    adw_alert_dialog_add_response (alert, "update", _("Confirm"));
    adw_alert_dialog_set_response_appearance (alert, "update",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (alert, "cancel");
    adw_alert_dialog_set_close_response (alert, "cancel");
    g_object_set_data (G_OBJECT (alert), "nuvasha-update-run", run);
    adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_update_chosen,
                             g_object_ref (self));
  }
}

/* The newer version Modrinth has for one file, or nothing when there is
 * none. Either way the check goes on to the next file. */
static void
on_update_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  PageWatch *watch = user_data;
  NuvashaContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  NuvashaModrinthVersion *update = NULL;
  NuvashaContentFile *file = NULL;
  UpdateRun *run = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  update = nuvasha_modrinth_update_by_hash_finish (result, &error);
  file = g_object_get_data (G_OBJECT (self), "nuvasha-update-file");
  run = g_object_get_data (G_OBJECT (self), "nuvasha-update-run");

  if (run == NULL)
    {
      nuvasha_modrinth_version_free (update);
      page_watch_free (watch);
      return;
    }

  /* The update endpoint answers with the newest version that fits, which
     is still the installed one when the file is already the newest: a
     version that carries the very file on disk is not an update. */
  if (update != NULL && file != NULL &&
      !nuvasha_modrinth_version_has_hash (update, file->sha1))
    {
      UpdateItem *item = g_new0 (UpdateItem, 1);

      item->version = update; /* owned from here */
       {
         const char *title = file->project_id != NULL
                               ? g_hash_table_lookup (self->browse.model->project_titles,
                                                      file->project_id)
                               : NULL;

         /* Update dialogs use the project name, never the version label.
          * A filename is the safest fallback while its project metadata is
          * still being resolved. */
         item->title = g_strdup (title != NULL ? title : file->filename);
       }
      item->path = g_strdup (file->path);
      item->disabled = !file->enabled;
      g_ptr_array_add (run->items, item);
    }
  else
    {
      /* Nothing newer, or the question could not be answered: neither is
         an update to install. */
      nuvasha_modrinth_version_free (update);
      g_clear_error (&error);
    }

  update_check_next (run);
  page_watch_free (watch);
}

/* One file after another, so a folder of twenty does not open twenty
 * requests at once. */
static void
update_check_next (UpdateRun *run)
{
  NuvashaContentPage *self = run != NULL ? run->page : NULL;

  if (self == NULL)
    {
      update_run_free (run);
      return;
    }

  while (run->index < self->installed.files->len)
    {
      NuvashaContentFile *file = g_ptr_array_index (self->installed.files, run->index);

      run->index++;

      if (file->sha1 == NULL || file->project_id == NULL)
        continue;

      g_object_set_data (G_OBJECT (self), "nuvasha-update-file", file);
      {
        PageWatch *watch = page_watch_new (self);

        watch->operation = nuvasha_modrinth_update_by_hash_async (
          self->type, file->sha1, self->minecraft_version, self->loader,
          on_update_ready, watch);
        nuvasha_operation_follow (watch->operation,
                                           self->browse.model->cancellable);
      }
      return;
    }

  update_check_done (self, run);
}

void
nuvasha_content_page_on_update_all_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  UpdateRun *run = g_new0 (UpdateRun, 1);

  if (g_object_get_data (G_OBJECT (self), "nuvasha-update-run") != NULL ||
      self->install_model->updating)
    {
      g_free (run);
      return;
    }
  update_button_busy (self, TRUE);
  run->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
  run->items =
    g_ptr_array_new_with_free_func ((GDestroyNotify) update_item_free);
  g_object_set_data (G_OBJECT (self), "nuvasha-update-run", run);
  update_check_next (run);
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

static void
installed_filter_rows (NuvashaContentPage *self);

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

          adw_status_page_set_icon_name (ADW_STATUS_PAGE (GTK_WIDGET (status)),
                                         nuvasha_content_type_icon (type));
          adw_status_page_set_title (ADW_STATUS_PAGE (GTK_WIDGET (status)),
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

  g_signal_connect (self->stack, "notify::visible-child-name",
                    G_CALLBACK (nuvasha_content_page_on_stack_switched), self);

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
