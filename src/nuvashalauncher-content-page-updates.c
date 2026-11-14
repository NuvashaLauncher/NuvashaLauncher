/* Update discovery and batch installation for the content page. */
#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-install-model.h"
#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-operation.h"

#define UPDATE_CHECK_BATCH_SIZE 200

/* One file with an available update. */
typedef struct {
  NuvashaLauncherModrinthVersion *version; /* the newer version, owned */
  char *title;
  char *path;
  gboolean disabled;
} UpdateItem;

/* One installed file eligible for a batch update check. */
typedef struct {
  char *sha1;
  char *project_id;
  char *filename;
  char *path;
  gboolean enabled;
} UpdateCandidate;

/* State for an update check. */
typedef struct {
  NuvashaLauncherContentPage *page; /* weakly held */
  guint index;
  guint batch_end;
  gboolean had_error;
  GPtrArray *candidates; /* UpdateCandidate* */
  GPtrArray *items;      /* UpdateItem* */
} UpdateRun;

static void update_check_next (UpdateRun *run);

static void
update_candidate_free (UpdateCandidate *candidate)
{
  if (candidate == NULL)
    return;

  g_free (candidate->sha1);
  g_free (candidate->project_id);
  g_free (candidate->filename);
  g_free (candidate->path);
  g_free (candidate);
}

static void
update_run_free (UpdateRun *run)
{
  if (run == NULL)
    return;

  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page),
                                  (gpointer *) &run->page);
  g_clear_pointer (&run->candidates, g_ptr_array_unref);
  g_clear_pointer (&run->items, g_ptr_array_unref);
  g_free (run);
}
  /* Update All: collect updates, confirm once, then install them. */

static void
update_item_free (UpdateItem *item)
{
  if (item == NULL)
    return;

  nuvashalauncher_modrinth_version_free (item->version);
  g_free (item->title);
  g_free (item->path);
  g_free (item);
}

static void
position_update_popover (NuvashaLauncherContentPage *self, GtkButton *button)
{
  graphene_rect_t bounds;
  GdkRectangle pointing_to;

  if (self->installed.update_popover == NULL)
    return;

  if (!gtk_widget_compute_bounds (GTK_WIDGET (button),
                                  GTK_WIDGET (self->installed.actions),
                                  &bounds))
    return;

  pointing_to = (GdkRectangle) {
    (int) graphene_rect_get_x (&bounds),
    (int) graphene_rect_get_y (&bounds),
    (int) graphene_rect_get_width (&bounds),
    (int) graphene_rect_get_height (&bounds)
  };
  gtk_popover_set_pointing_to (self->installed.update_popover, &pointing_to);
}

static void
update_button_busy (NuvashaLauncherContentPage *self, gboolean busy)
{
  if (self->installed.update_content == NULL)
    return;
  gtk_stack_set_visible_child_name (self->installed.update_content,
                                    busy ? "spinner" : "icon");
  gtk_widget_set_sensitive (
    GTK_WIDGET (self->installed.update_all),
    self->installed.files != NULL && self->installed.files->len > 0);
   gtk_widget_set_can_target (GTK_WIDGET (self->installed.update_all), TRUE);
  gtk_widget_set_focusable (GTK_WIDGET (self->installed.update_all), TRUE);
  if (self->installed.update_cancel != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->installed.update_cancel), busy);
}

void
nuvashalauncher_content_page_on_update_cancel (GtkButton *button G_GNUC_UNUSED,
                                       gpointer user_data)
{
  NuvashaLauncherContentPage *self = user_data;
  UpdateRun *run = g_object_get_data (G_OBJECT (self), "nuvashalauncher-update-run");

  if (run != NULL)
    {
      g_object_set_data (G_OBJECT (self), "nuvashalauncher-update-run", NULL);
      update_run_free (run);
    }
  g_cancellable_cancel (self->browse.model->cancellable);
  g_clear_object (&self->browse.model->cancellable);
  self->browse.model->cancellable = g_cancellable_new ();
  g_clear_pointer (&self->install_model->updates, g_ptr_array_unref);
  self->install_model->update_index = 0;
  self->install_model->updating = FALSE;
  update_button_busy (self, FALSE);
  if (self->installed.update_popover != NULL)
    gtk_popover_popdown (self->installed.update_popover);
}

static void
update_progress (NuvashaLauncherContentPage *self, guint done, guint total)
{
  g_autofree char *count = g_strdup_printf ("%u/%u", done, total);

  if (self->installed.update_phase != NULL)
    gtk_label_set_text (self->installed.update_phase, _("Progress"));
  if (self->installed.update_count != NULL)
    gtk_label_set_text (self->installed.update_count, count);
  if (self->installed.update_bar != NULL)
    gtk_progress_bar_set_fraction (self->installed.update_bar,
                                   total > 0 ? (double) done / total : 0.0);
}

/* Starts the next one of a confirmed batch. When the last one is in, the
 * list is read again and the batch is over. */
void
update_batch_next (NuvashaLauncherContentPage *self)
{
  UpdateItem *item = NULL;

  if (self->install_model->updates == NULL || self->install_model->update_index >= self->install_model->updates->len)
    {
      guint done = self->install_model->updates != NULL ? self->install_model->updates->len : 0;

      g_clear_pointer (&self->install_model->updates, g_ptr_array_unref);
      self->install_model->update_index = 0;
      self->install_model->updating = FALSE;
      update_button_busy (self, FALSE);
      if (self->installed.update_popover != NULL)
        gtk_popover_popdown (self->installed.update_popover);

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
  update_progress (self, self->install_model->update_index,
                   self->install_model->updates->len);
  self->install_model->update_index++;

  update_button_busy (self, TRUE);
  start_plan (self, nuvashalauncher_modrinth_version_copy (item->version), item->title,
              item->path, item->disabled);
}

void
plan_stopped (NuvashaLauncherContentPage *self)
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
  NuvashaLauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  UpdateRun *run = g_object_get_data (G_OBJECT (source), "nuvashalauncher-update-run");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (run == NULL || self == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (g_strcmp0 (response, "update") != 0)
    {
      g_object_set_data (G_OBJECT (self), "nuvashalauncher-update-run", NULL);
      update_run_free (run);
      g_object_unref (self);
      return;
    }

  /* The whole list was confirmed: install each one. */
  self->install_model->updates = g_steal_pointer (&run->items);
  self->install_model->update_index = 0;
  self->install_model->updating = TRUE;
  g_object_set_data (G_OBJECT (self), "nuvashalauncher-update-run", NULL);
  update_run_free (run);
  update_batch_next (self);

  g_object_unref (self);
}

/* Finish the update check and show the confirmation dialog or an up-to-date
 * message. */
static void
update_check_done (NuvashaLauncherContentPage *self, UpdateRun *run)
{
  update_button_busy (self, FALSE);
  if (self->installed.update_popover != NULL)
    gtk_popover_popdown (self->installed.update_popover);
  g_object_set_data (G_OBJECT (self), "nuvashalauncher-update-run", NULL);

  if (run->items->len == 0)
    {
      if (!run->had_error)
        show_toast (self, _("Everything is up to date"));
      update_run_free (run);
      return;
    }

  {
    const char *what = self->type == nuvashalauncher_CONTENT_MOD
                         ? "mods"
                         : self->type == nuvashalauncher_CONTENT_RESOURCE_PACK
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
    g_object_set_data (G_OBJECT (alert), "nuvashalauncher-update-run", run);
    adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_update_chosen,
                             g_object_ref (self));
  }
}

/* The newer versions Modrinth has for a batch of file hashes. */
static void
on_updates_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  PageWatch *watch = user_data;
  NuvashaLauncherContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) updates = NULL;
  UpdateRun *run = NULL;

  updates = nuvashalauncher_modrinth_updates_by_hashes_finish (result, &error);

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  run = g_object_get_data (G_OBJECT (self), "nuvashalauncher-update-run");

  if (run == NULL)
    {
      page_watch_free (watch);
      return;
    }

  if (updates != NULL)
    {
      for (guint i = run->index; i < run->batch_end; i++)
        {
          UpdateCandidate *cand = g_ptr_array_index (run->candidates, i);
          NuvashaLauncherModrinthVersion *update =
            g_hash_table_lookup (updates, cand->sha1);

          /* The update endpoint answers with the newest version that fits,
             which is still the installed one when the file is already the
             newest: a version that carries the very file on disk is not an
             update. */
          if (update != NULL &&
              !nuvashalauncher_modrinth_version_has_hash (update, cand->sha1))
            {
              UpdateItem *item = g_new0 (UpdateItem, 1);
              const char *title =
                cand->project_id != NULL
                  ? g_hash_table_lookup (self->browse.model->project_titles,
                                         cand->project_id)
                  : NULL;

              item->version = nuvashalauncher_modrinth_version_copy (update);
              /* Update dialogs use the project name, never the version label.
               * A filename is the safest fallback while its project metadata is
               * still being resolved. */
              item->title = g_strdup (title != NULL ? title : cand->filename);
              item->path = g_strdup (cand->path);
              item->disabled = !cand->enabled;
              g_ptr_array_add (run->items, item);
            }
        }
    }
  else if (error != NULL &&
           !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      run->had_error = TRUE;
      show_toast (self, error->message);
    }

  run->index = run->batch_end;
  update_progress (self, run->index, run->candidates->len);
  update_check_next (run);
  page_watch_free (watch);
}

/* Check installed files in batches via Modrinth's bulk hash-update endpoint. */
static void
update_check_next (UpdateRun *run)
{
  NuvashaLauncherContentPage *self = run != NULL ? run->page : NULL;
  g_autoptr (GPtrArray) hashes = NULL;
  g_autoptr (GHashTable) seen = NULL;
  guint end = 0;

  if (self == NULL || run->candidates == NULL)
    {
      update_run_free (run);
      return;
    }

  if (run->index >= run->candidates->len)
    {
      update_check_done (self, run);
      return;
    }

  end = MIN (run->index + UPDATE_CHECK_BATCH_SIZE, run->candidates->len);
  run->batch_end = end;
  hashes = g_ptr_array_new ();
  seen = g_hash_table_new (g_str_hash, g_str_equal);

  for (guint i = run->index; i < end; i++)
    {
      UpdateCandidate *cand = g_ptr_array_index (run->candidates, i);

      if (g_hash_table_add (seen, cand->sha1))
        g_ptr_array_add (hashes, cand->sha1);
    }

  update_progress (self, run->index, run->candidates->len);

  {
    PageWatch *watch = page_watch_new (self);

    watch->operation = nuvashalauncher_modrinth_updates_by_hashes_async (
      self->type, hashes, self->minecraft_version, self->loader,
      on_updates_ready, watch);
    nuvashalauncher_operation_follow (watch->operation,
                                       self->browse.model->cancellable);
  }
}

void
nuvashalauncher_content_page_on_update_all_clicked (GtkButton *button,
                                                    gpointer user_data)
{
  NuvashaLauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  UpdateRun *run = g_new0 (UpdateRun, 1);

  position_update_popover (self, button);

  if (g_object_get_data (G_OBJECT (self), "nuvashalauncher-update-run") != NULL ||
      self->install_model->updating)
    {
      if (self->installed.update_popover != NULL)
        gtk_popover_popup (self->installed.update_popover);
      g_free (run);
      return;
    }
  update_button_busy (self, TRUE);
  run->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
  run->candidates =
    g_ptr_array_new_with_free_func ((GDestroyNotify) update_candidate_free);
  run->items =
    g_ptr_array_new_with_free_func ((GDestroyNotify) update_item_free);

  if (self->installed.files != NULL)
    {
      for (guint i = 0; i < self->installed.files->len; i++)
        {
          NuvashaLauncherContentFile *file =
            g_ptr_array_index (self->installed.files, i);

          if (file->sha1 == NULL || file->project_id == NULL)
            continue;

          {
            UpdateCandidate *cand = g_new0 (UpdateCandidate, 1);

            cand->sha1 = g_strdup (file->sha1);
            cand->project_id = g_strdup (file->project_id);
            cand->filename = g_strdup (file->filename);
            cand->path = g_strdup (file->path);
            cand->enabled = file->enabled;
            g_ptr_array_add (run->candidates, cand);
          }
        }
    }

  g_object_set_data (G_OBJECT (self), "nuvashalauncher-update-run", run);
  if (self->installed.update_popover != NULL)
    gtk_popover_popup (self->installed.update_popover);
  update_check_next (run);
}
