/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Content selection and batch-install workflow. Individual install planning
 * and downloads remain in leppoisa-content-page-install.c. */

#include "leppoisa-content-page-private.h"
#include "leppoisa-i18n.h"
#include "leppoisa-modrinth.h"

/* ---------------------------------------------------------- *
 * What the user picked
 *
 * A version whose checkmark is on joins a list, and the button at the
 * foot of the list installs the whole of it at once. A project is one
 * entry of that list: picking another version of a project that is
 * already on it replaces the one that was there, because installing the
 * same project twice would put two files in the folder.
 *
 * The list is not a download and does not stop anything: it waits as
 * long as the page lives, and the confirmation is asked once for the
 * whole of it, the way Update All asks once for every update it found.
 * ---------------------------------------------------------- */

/* One entry of the list: which version of which project, and the words
   the confirmation and the download call it by. */
struct _Pick {
  char *project_id;
  char *title;
  LeppoisaModrinthVersion *version; /* owned */
};

void
pick_free (Pick *pick)
{
  if (pick == NULL)
    return;

  leppoisa_modrinth_version_free (pick->version);
  g_free (pick->project_id);
  g_free (pick->title);
  g_free (pick);
}

/* The entry of @project_id in the list, or NULL when the project has
   nothing picked. */
static Pick *
pick_of_project (LeppoisaContentPage *self, const char *project_id)
{
  if (self->install_model->picks == NULL || project_id == NULL)
    return NULL;

  for (guint i = 0; i < self->install_model->picks->len; i++)
    {
      Pick *pick = g_ptr_array_index (self->install_model->picks, i);

      if (g_strcmp0 (pick->project_id, project_id) == 0)
        return pick;
    }

  return NULL;
}

/* Whether @pick holds @version itself rather than another version of the
   same project: the copy in the list is not the one the page shows, so
   the ids are what say they are the same. */
static gboolean
pick_is_version (Pick *pick, LeppoisaModrinthVersion *version)
{
  if (pick == NULL || version == NULL || pick->version == NULL)
    return FALSE;

  return g_strcmp0 (pick->version->version_id, version->version_id) == 0;
}

/* The checkmark at the left of the versions follows the list: the version
   the dropdown shows is on the list, and the button says so. Its state is
   set with the click handler held back, which is what keeps the two from
   calling each other. */
void
refresh_pick_check (LeppoisaContentPage *self)
{
  LeppoisaModrinthVersion *version = NULL;
  gboolean on;

  if (self->project.details_check == NULL || self->project.details_dropdown == NULL)
    return;

  version = selected_version (self->project.details_dropdown,
                              self->project.details_versions);
  on = pick_is_version (pick_of_project (
                          self, version != NULL ? version->project_id : NULL),
                        version);

  g_object_set_data (G_OBJECT (self->project.details_check), "leppoisa-setting",
                     GINT_TO_POINTER (1));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->project.details_check), on);
  g_object_set_data (G_OBJECT (self->project.details_check), "leppoisa-setting", NULL);
}

/* What the page has picked is said to the header, which counts it on the
   icon of its downloads button, and put on the button at the foot of the
   list. Called after every change of the list, and once when the list is
   built, so that the button starts where the picks start. */
void
leppoisa_content_page_refresh_picks (LeppoisaContentPage *self)
{
  guint count = self->install_model->picks != NULL ? self->install_model->picks->len : 0;
  if (self->pick_badge != NULL)
    {
      g_autofree char *number = g_strdup_printf (
          "%" G_GUINT64_FORMAT, (guint64) count);

      gtk_label_set_text (GTK_LABEL (self->pick_badge),
                          count > 0 ? number : "");
      gtk_widget_set_visible (GTK_WIDGET (self->pick_badge), count > 0);
      gtk_accessible_update_property (GTK_ACCESSIBLE (self->pick_badge),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      number, -1);
    }

  if (self->pick_button != NULL)
    {
      g_autofree char *tooltip = NULL;

      if (count > 0)
        tooltip = g_strdup_printf (_("Install the selected %s?"),
                                   leppoisa_content_type_title (self->type));

      gtk_widget_set_sensitive (GTK_WIDGET (self->pick_button), count > 0);
      gtk_widget_set_tooltip_text (
           GTK_WIDGET (self->pick_button),
           count == 0 ? _("Nothing is selected")
                      : tooltip);
    }

  refresh_pick_check (self);
}

/* @version of @project_id is now picked: it replaces whatever that
   project had on the list, or joins it. */
void
pick_add (LeppoisaContentPage *self, LeppoisaModrinthVersion *version)
{
  Pick *pick = NULL;

  if (self->install_model->picks == NULL || version == NULL || version->project_id == NULL)
    return;

  pick = pick_of_project (self, version->project_id);

  if (pick != NULL)
    {
      /* The very version that was already on the list is left alone, so
         the checkmarks do not move under the click that put it there. */
      if (pick_is_version (pick, version))
        return;

      g_ptr_array_remove (self->install_model->picks, pick);
    }

  pick = g_new0 (Pick, 1);
  pick->project_id = g_strdup (version->project_id);
  pick->title = g_strdup (self->project.current_project_title != NULL
                            ? self->project.current_project_title
                            : version_name_of (version));
  pick->version = leppoisa_modrinth_version_copy (version);
  g_ptr_array_add (self->install_model->picks, pick);
}

/* The project of @version has nothing picked any more. Only its own
   entry goes: another version of the same project is a different pick. */
void
pick_remove (LeppoisaContentPage *self, LeppoisaModrinthVersion *version)
{
  Pick *pick = NULL;

  if (self->install_model->picks == NULL || version == NULL)
    return;

  pick = pick_of_project (self, version->project_id);

  if (pick_is_version (pick, version))
    g_ptr_array_remove (self->install_model->picks, pick);
}

/* What the confirmation is answered with. The list was taken out of the
   page when the question was asked, so what is installed is the list the
   user saw and nothing that is picked since. */
static void
on_batch_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LeppoisaContentPage *self = leppoisa_CONTENT_PAGE (user_data);
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "install") != 0)
    {
      /* Nothing was installed, so the list the question was about goes
         back to the page with its checkmarks as they were. */
      if (self->install_model->batch != NULL)
        {
          g_clear_pointer (&self->install_model->picks, g_ptr_array_unref);
          self->install_model->picks = g_steal_pointer (&self->install_model->batch);
          leppoisa_content_page_refresh_picks (self);
        }

      g_object_unref (self);
      return;
    }

  if (self->install_model->batch == NULL)
    {
      g_object_unref (self);
      return;
    }

  self->install_model->picking = TRUE;
  self->install_model->pick_index = 0;
  batch_next (self);

  g_object_unref (self);
}

/* Starts the next version of the confirmed list, or says that the whole
   of it is in. Each one is planned and fetched like any other install;
   the confirmation is not asked again for the ones that follow. */
void
batch_next (LeppoisaContentPage *self)
{
  Pick *pick = NULL;

  if (self->install_model->batch == NULL || self->install_model->pick_index >= self->install_model->batch->len)
    {
      guint done = self->install_model->batch != NULL ? self->install_model->batch->len : 0;

      g_clear_pointer (&self->install_model->batch, g_ptr_array_unref);
      self->install_model->picking = FALSE;
      self->install_model->pick_index = 0;
      set_busy (self, FALSE);

      if (done > 0)
        {
          g_autofree char *message = g_strdup_printf (
            ngettext ("Installed %u item", "Installed %u items", done), done);

          show_toast (self, message);
          reload_installed (self);
        }

      return;
    }

  pick = g_ptr_array_index (self->install_model->batch, self->install_model->pick_index);
  self->install_model->pick_index++;

  set_busy (self, TRUE);
  start_plan (self, leppoisa_modrinth_version_copy (pick->version), pick->title,
              NULL, FALSE);
}

/* A file of the batch could not be installed: what it would take is not
   there, so the list goes no further. */
void
batch_stopped (LeppoisaContentPage *self)
{
  g_clear_pointer (&self->install_model->batch, g_ptr_array_unref);
  self->install_model->picking = FALSE;
  self->install_model->pick_index = 0;
}

void
leppoisa_content_page_clear_picks (LeppoisaContentPage *self)
{
  g_clear_pointer (&self->install_model->picks, g_ptr_array_unref);
  self->install_model->picks = g_ptr_array_new_with_free_func ((GDestroyNotify) pick_free);
  leppoisa_content_page_refresh_picks (self);
}

/* The button at the foot of the list: every picked version, one per
   line, and the choice to install the whole of it at once. */
void
leppoisa_content_page_on_pick_button_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  LeppoisaContentPage *self = leppoisa_CONTENT_PAGE (user_data);
  g_autoptr (GString) body = NULL;
  g_autoptr (GHashTable) listed = NULL;
  AdwAlertDialog *alert = NULL;
  const char *kind = NULL;

  if (self->install_model->picks == NULL || self->install_model->picks->len == 0)
    return;

  body = g_string_new (NULL);
  listed = g_hash_table_new (g_str_hash, g_str_equal);

  kind = leppoisa_content_type_title (self->type);

  for (guint i = 0; i < self->install_model->picks->len; i++)
    {
      Pick *pick = g_ptr_array_index (self->install_model->picks, i);

      if (i > 0)
        g_string_append_c (body, '\n');

      g_string_append (body, pick->title);
      g_hash_table_add (listed, pick->project_id);
    }

  /* Show the required dependencies in their own section. These are the
     dependencies published for the selected versions, not optional or
     incompatible projects. The resolver still chooses the compatible
     version when the install starts. */
  {
    gboolean has_dependencies = FALSE;
    GString *dependencies = g_string_new (NULL);

    for (guint i = 0; i < self->install_model->picks->len; i++)
      {
        Pick *pick = g_ptr_array_index (self->install_model->picks, i);

        for (guint j = 0; pick->version->dependencies != NULL &&
                         j < pick->version->dependencies->len; j++)
          {
            LeppoisaModrinthDependency *dependency =
              g_ptr_array_index (pick->version->dependencies, j);
            const char *title = NULL;

            if (dependency->type != leppoisa_DEPENDENCY_REQUIRED ||
                dependency->project_id == NULL ||
                g_hash_table_contains (listed, dependency->project_id))
              continue;

            g_hash_table_add (listed, dependency->project_id);
            title = g_hash_table_lookup (self->browse.model->project_titles,
                                         dependency->project_id);
            if (has_dependencies)
              g_string_append_c (dependencies, '\n');
            g_string_append_printf (dependencies, "%s",
                                    title != NULL ? title
                                                  : dependency->project_id);
            has_dependencies = TRUE;
          }
      }

    if (has_dependencies)
      {
        g_string_append_printf (body, "\n%s\n", _("Dependencies:"));
        g_string_append (body, dependencies->str);
      }

    g_string_free (dependencies, TRUE);
    }

  {
    g_autofree char *heading =
      g_strdup_printf (_("Install the selected %s?"), kind);

    alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body->str));
  }

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "install", _("Install"));
  adw_alert_dialog_set_response_appearance (alert, "install",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  /* The list goes into the batch here: what is installed is the list
     this question is about, whatever is picked while it is up. */
  g_clear_pointer (&self->install_model->batch, g_ptr_array_unref);
  self->install_model->batch = g_steal_pointer (&self->install_model->picks);
  self->install_model->picks = g_ptr_array_new_with_free_func ((GDestroyNotify) pick_free);
  leppoisa_content_page_refresh_picks (self);

  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_batch_chosen,
                           g_object_ref (self));
}
