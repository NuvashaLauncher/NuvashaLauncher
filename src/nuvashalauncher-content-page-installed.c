/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-index.h"
#include "nuvashalauncher-content-format.h"
#include "nuvashalauncher-content-files-private.h"
#include "nuvashalauncher-task.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-icon-load-private.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-i18n.h"

/* Installed-content rows, selection, deletion, and icon loading. */

void
show_error (NuvashaLauncherContentPage *self, const char *what, GError *error)
{
  if (self->banner == NULL)
    return;

  nuvashalauncher_ui_error_banner (self->banner, error);
  g_autofree char *message = g_strdup_printf (
    "%s: %s", what, nuvashalauncher_error_message (error));
  adw_banner_set_title (ADW_BANNER (self->banner), message);
}

/* Remove all rows from a preferences group. */
void
take_rows_out (GList **rows, AdwPreferencesGroup *group)
{
  for (GList *item = *rows; item != NULL; item = item->next)
    {
      g_object_set_data (G_OBJECT (item->data), "nuvashalauncher-file", NULL);
      if (group != NULL)
        adw_preferences_group_remove (group, item->data);
      g_object_unref (item->data);
    }

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
gboolean
nuvashalauncher_content_page_toggle_file (NuvashaLauncherContentPage *self,
                                          NuvashaLauncherContentFile *file)
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

  {
    g_autofree char *old_base = g_path_get_basename (old_path);
    g_autofree char *new_base = g_path_get_basename (file->path);
    g_autofree char *directory = NULL;

    g_free (file->filename);
    file->filename = g_steal_pointer (&new_base);
    if (self->instance != NULL)
      {
        directory = content_page_base_directory (self);
        if (directory != NULL)
          {
            nuvashalauncher_task_scope_release_when_done (
              nuvashalauncher_content_index_remove_async (directory, self->type,
                                                          old_base, NULL, NULL),
              self->installed.scan);
            if (file->sha1 != NULL)
              persist_identified_file (self, file);
          }
      }
  }

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
const char *
nuvashalauncher_content_page_file_display_name (NuvashaLauncherContentFile *file)
{
  if (file != NULL && file->project_name != NULL &&
      *file->project_name != '\0')
    return file->project_name;

  return file != NULL ? file->filename : NULL;
}

static void
installed_filter_row (GtkWidget *row, const char *needle)
{
  NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
  gboolean visible = TRUE;

  if (needle != NULL && *needle != '\0')
    visible = file != NULL && file->search_key != NULL &&
              strstr (file->search_key, needle) != NULL;

  gtk_widget_set_visible (row, visible);
}

static char *
installed_search_needle (NuvashaLauncherContentPage *self)
{
  const char *query = self != NULL && self->installed.search_entry != NULL
                        ? gtk_editable_get_text (
                            GTK_EDITABLE (self->installed.search_entry))
                        : "";

  if (query == NULL || *query == '\0')
    return NULL;

  return g_utf8_casefold (query, -1);
}

void
installed_filter_rows (NuvashaLauncherContentPage *self)
{
  g_autofree char *needle = installed_search_needle (self);

  for (GList *item = self->installed.rows_list; item != NULL; item = item->next)
    installed_filter_row (GTK_WIDGET (item->data), needle);
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
  NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
  NuvashaLauncherContentPage *self =
    NUVASHALAUNCHER_CONTENT_PAGE (g_object_get_data (G_OBJECT (row), "nuvashalauncher-page"));
  const char *title = NULL;

  if (file == NULL || !GTK_IS_WIDGET (row))
    return;

  nuvashalauncher_content_file_refresh_search_key (file);

  {
    GtkWidget *trash = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file-trash");
    GtkWidget *sw = g_object_get_data (G_OBJECT (row), "nuvashalauncher-mod-switch");
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

  {
    g_autofree char *needle = installed_search_needle (self);
    installed_filter_row (row, needle);
  }
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
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (sw), "nuvashalauncher-file");

  if (file == NULL || !nuvashalauncher_content_page_toggle_file (self, file))
    return TRUE;

  gtk_widget_set_tooltip_text (GTK_WIDGET (sw),
                                file->enabled ? _("Disable") : _("Enable"));

  return FALSE;
}

GtkWidget *
file_row (NuvashaLauncherContentPage *self, NuvashaLauncherContentFile *file)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_CONTENT_PAGE (self), NULL);
  g_return_val_if_fail (file != NULL, NULL);

  AdwActionRow *row = ADW_ACTION_ROW (g_object_ref_sink (adw_action_row_new ()));
  gtk_widget_add_css_class (GTK_WIDGET (row), "nuvashalauncher-content-file-row");
  GtkWidget *icon = nuvashalauncher_icon_widget_new_pack_placeholder ();

  /* Treat file names and paths as plain text. The tooltip contains the path;
     the subtitle contains project metadata when available. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (
    ADW_PREFERENCES_ROW (row),
    nuvashalauncher_content_page_file_display_name (file));
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), FALSE);
  if (file->description != NULL && *file->description != '\0')
    row_show_description (GTK_WIDGET (row), file->description);

  /* Use a fixed icon square and replace the placeholder when project data
     becomes available. */
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-icon", icon);

  /* Only a mod is something that can be turned off: a resource pack or a
     shader pack is loaded whenever it is present, so neither has a
     switch. */
  if (self->type == NUVASHALAUNCHER_CONTENT_MOD)
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

  /* Every kind of file can be deleted. On a mod the button sits after
     the switch, at the right end of the row. */
  {
    GtkButton *trash = GTK_BUTTON (gtk_button_new_from_icon_name ("edit-delete-symbolic"));

     nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (trash), _("Delete"));
    /* Do not let clicking delete move keyboard focus into a row that is
     * about to disappear. GTK would otherwise focus the first remaining row
     * when this button is removed, which scrolls the list to the top. */
    gtk_widget_set_focus_on_click (GTK_WIDGET (trash), FALSE);
    gtk_widget_set_valign (GTK_WIDGET (trash), GTK_ALIGN_CENTER);
     g_object_set_data (G_OBJECT (trash), "nuvashalauncher-file", file);
     g_object_set_data (G_OBJECT (row), "nuvashalauncher-file-trash", trash);
     g_signal_connect_object (trash, "clicked",
                              G_CALLBACK (nuvashalauncher_content_page_on_trash_clicked),
                              self, 0);
     adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (trash));
  }
  /* The row carries the file it is about, so the switch does not have to
     know where the file came from. */
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-file", file);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-page", self);

  file_row_refresh (GTK_WIDGET (row));

  return GTK_WIDGET (row);
}

typedef struct {
  NuvashaLauncherContentPage *page;
  GtkWidget *row;
} LocalIconLoad;

static void
local_icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  LocalIconLoad *load = user_data;
  NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (load->row), "nuvashalauncher-file");
  g_autoptr (GError) error = NULL;
  g_autofree char *path =
    nuvashalauncher_content_load_local_icon_finish (result, &error);
  gboolean file_valid = FALSE;

  g_object_set_data (G_OBJECT (load->row), "nuvashalauncher-local-icon-loading", NULL);

  file_valid = file != NULL && file->path != NULL &&
               load->page->installed.rows != NULL &&
               g_hash_table_lookup (load->page->installed.rows, file->path) == load->row;

  if (path != NULL && file_valid)
    {
      g_free (file->local_icon_path);
      file->local_icon_path = g_steal_pointer (&path);

      if (load->page->installed.scroller != NULL)
        {
          GtkWidget *icon = GTK_WIDGET (
            g_object_get_data (G_OBJECT (load->row), "nuvashalauncher-icon"));
          if (icon != NULL)
            nuvashalauncher_icon_widget_set_file (icon, file->local_icon_path);
        }
      queue_persist_identified_file (load->page, file);
    }

  g_object_unref (load->row);
  g_object_unref (load->page);
  g_free (load);
}

static void
load_row_icon (NuvashaLauncherContentPage *self, GtkWidget *row, int scroller_height)
{
  NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
  GtkWidget *icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (row), "nuvashalauncher-icon"));
  GtkWidget *scroller = GTK_WIDGET (self->installed.scroller);
  graphene_rect_t bounds;

  if (file == NULL || icon == NULL || scroller == NULL ||
      !gtk_widget_get_visible (row) ||
      (file->local_icon_path == NULL && file->local_icon_member == NULL &&
       (file->icon_url == NULL || *file->icon_url == '\0')) ||
      g_object_get_data (G_OBJECT (row), "nuvashalauncher-icon-loaded") != NULL ||
      !gtk_widget_compute_bounds (row, scroller, &bounds))
    return;

  if (bounds.origin.y + bounds.size.height < 0 ||
      bounds.origin.y > scroller_height)
    return;

  if (file->local_icon_path != NULL &&
      nuvashalauncher_icon_widget_set_file (icon, file->local_icon_path))
    {
      g_object_set_data (G_OBJECT (row), "nuvashalauncher-icon-loaded", GINT_TO_POINTER (1));
      return;
    }

  g_clear_pointer (&file->local_icon_path, g_free);
  if (file->local_icon_member != NULL &&
      g_object_get_data (G_OBJECT (row), "nuvashalauncher-local-icon-loading") == NULL)
    {
      LocalIconLoad *load = g_new0 (LocalIconLoad, 1);

      load->page = g_object_ref (self);
      load->row = g_object_ref (row);
      g_object_set_data (G_OBJECT (row), "nuvashalauncher-local-icon-loading",
                         GINT_TO_POINTER (1));

      nuvashalauncher_task_scope_release_when_done (
        nuvashalauncher_content_load_local_icon_async (file, local_icon_ready, load),
        self->installed.scan);
      return;
    }

  if (file->icon_url != NULL && *file->icon_url != '\0')
    {
      g_object_set_data (G_OBJECT (row), "nuvashalauncher-icon-loaded", GINT_TO_POINTER (1));
      nuvashalauncher_icon_load_start (icon, file->icon_url);
    }
}

void
load_visible_icons (NuvashaLauncherContentPage *self)
{
  int scroller_height = self->installed.scroller != NULL
                          ? gtk_widget_get_height (GTK_WIDGET (self->installed.scroller))
                          : 0;

  if (scroller_height <= 0)
    return;

  for (GList *item = self->installed.rows_list; item != NULL; item = item->next)
    load_row_icon (self, item->data, scroller_height);
}

void
nuvashalauncher_content_page_on_installed_scroll_changed (
  GtkAdjustment *adjustment G_GNUC_UNUSED, gpointer user_data)
{
  load_visible_icons (user_data);
}
