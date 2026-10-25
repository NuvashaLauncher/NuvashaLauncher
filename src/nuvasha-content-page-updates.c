/* Update discovery and batch installation for the content page. */
#include "nuvasha-content-page-private.h"
#include "nuvasha-content-install-model.h"
#include "nuvasha-modrinth.h"
#include "nuvasha-i18n.h"
#include "nuvasha-operation.h"

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

static void update_check_next (UpdateRun *run);

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
