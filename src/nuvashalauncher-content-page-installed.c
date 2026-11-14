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

  /* The row the file is about follows the new path, and the table that
     finds a row by its path is told, so nothing else has to move. */
  row = g_hash_table_lookup (self->installed.rows, old_path);

  if (row != NULL)
    {
      GtkWidget *check = g_object_get_data (G_OBJECT (row),
                                            "nuvashalauncher-file-check");

      g_hash_table_remove (self->installed.rows, old_path);
      g_hash_table_insert (self->installed.rows, g_strdup (file->path), row);
      if (check != NULL)
        g_object_set_data_full (G_OBJECT (check), "nuvashalauncher-file-path",
                                g_strdup (file->path), g_free);
      if (self->installed.selected_paths != NULL &&
          g_hash_table_remove (self->installed.selected_paths, old_path))
        g_hash_table_add (self->installed.selected_paths,
                          g_strdup (file->path));
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

void
installed_filter_rows (NuvashaLauncherContentPage *self)
{
  const char *query = self->installed.search_entry != NULL
                        ? gtk_editable_get_text (
                            GTK_EDITABLE (self->installed.search_entry))
                        : "";
  g_autofree char *needle = g_utf8_casefold (query, -1);

  for (GList *item = self->installed.rows_list; item != NULL; item = item->next)
    {
      GtkWidget *row = GTK_WIDGET (item->data);
      NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
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
  NuvashaLauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
  NuvashaLauncherContentPage *self =
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
  NuvashaLauncherContentPage *self = user_data;
  const char *path = g_object_get_data (G_OBJECT (check), "nuvashalauncher-file-path");
  GtkStack *stack = g_object_get_data (G_OBJECT (check), "nuvashalauncher-icon-stack");

  if (stack != NULL)
    gtk_stack_set_visible_child_name (stack,
      gtk_check_button_get_active (check) ? "check" : "icon");

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
content_page_update_selection (NuvashaLauncherContentPage *self)
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
  NuvashaLauncherContentPage *self = user_data;
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
  NuvashaLauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
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
    ADW_PREFERENCES_ROW (row),
    nuvashalauncher_content_page_file_display_name (file));
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), FALSE);
  if (file->description != NULL && *file->description != '\0')
    row_show_description (GTK_WIDGET (row), file->description);

  /* Use a fixed icon square and replace the placeholder when project data
     becomes available. */
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);

  GtkStack *icon_stack = GTK_STACK (gtk_stack_new ());
  GtkBox *badge_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkImage *check_icon = GTK_IMAGE (gtk_image_new_from_icon_name ("object-select-symbolic"));

  gtk_widget_add_css_class (GTK_WIDGET (badge_box), "nuvashalauncher-content-icon");
  gtk_widget_add_css_class (GTK_WIDGET (badge_box), "nuvashalauncher-select-badge");
  gtk_widget_set_size_request (GTK_WIDGET (badge_box), nuvashalauncher_ICON_SIZE, nuvashalauncher_ICON_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (badge_box), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (badge_box), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (badge_box), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (badge_box), TRUE);

  gtk_image_set_pixel_size (check_icon, 24);
  gtk_widget_set_valign (GTK_WIDGET (check_icon), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (check_icon), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (check_icon), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (check_icon), TRUE);
  gtk_box_append (badge_box, GTK_WIDGET (check_icon));

  gtk_stack_add_named (icon_stack, GTK_WIDGET (icon), "icon");
  gtk_stack_add_named (icon_stack, GTK_WIDGET (badge_box), "check");
  gtk_stack_set_visible_child_name (icon_stack, "icon");
  gtk_widget_set_size_request (GTK_WIDGET (icon_stack), nuvashalauncher_ICON_SIZE, nuvashalauncher_ICON_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (icon_stack), GTK_ALIGN_CENTER);

  gtk_check_button_set_child (check, GTK_WIDGET (icon_stack));
  gtk_widget_add_css_class (GTK_WIDGET (check), "nuvashalauncher-select-checkbox");
  gtk_widget_set_valign (GTK_WIDGET (check), GTK_ALIGN_CENTER);
  g_object_set_data (G_OBJECT (check), "nuvashalauncher-icon-stack", icon_stack);

  gtk_widget_set_visible (GTK_WIDGET (check), TRUE);
  g_object_set_data_full (G_OBJECT (check), "nuvashalauncher-file-path",
                          g_strdup (file->path), g_free);
  g_signal_connect (check, "toggled",
                    G_CALLBACK (on_content_file_check_toggled), self);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), GTK_WIDGET (check));
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
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_signal_connect (row, "activated",
                    G_CALLBACK (on_content_file_row_activated), self);

  file_row_refresh (GTK_WIDGET (row));

  return GTK_WIDGET (row);
}
