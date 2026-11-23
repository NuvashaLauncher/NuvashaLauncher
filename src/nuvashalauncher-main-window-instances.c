/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-instance-page.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-ui-feedback.h"

/* ---------------------------------------------------------- *
 * The dialogs of the instances
 *
 * An instance opens as an AdwDialog over the launcher, exactly like the
 * Settings dialog: it is not resizable, it cannot be moved away from the
 * window, and the page's own header bar becomes the header of the
 * dialog. That is why the instance page is not put in a window of its
 * own.
 * ---------------------------------------------------------- */

/* The dialog @id is open in, or NULL. The id travels with the dialog, so
   the table of open dialogs is all the bookkeeping there is: a dialog
   that is closed is no longer in it, and the one that holds the id is
   the one to present again. */
static AdwDialog *
dialog_of_instance (NuvashaLauncherMainWindow *self, const char *id)
{
  if (id == NULL)
    return NULL;

  return g_hash_table_lookup (self->model->instance_dialogs, id);
}

static gboolean
restore_instance_list_focus (gpointer user_data)
{
  NuvashaLauncherMainWindow *self = user_data;

  if (self->ui.instances != NULL)
    gtk_widget_grab_focus (GTK_WIDGET (self->ui.instances));

  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

/* A dialog that was closed is not open any more. AdwDialog emits this
   while it is still alive, so the borrowed pointer leaves the table
   before the dialog is freed. */
static void
on_instance_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  const char *id = g_object_get_data (G_OBJECT (dialog), "nuvashalauncher-instance-id");

  /* A closed dialog is removed from the reopen table. Detach its page now
   * as well, so large content lists and their decoded textures are released
   * immediately instead of waiting for the dialog object to be finalized. */
  adw_dialog_set_child (dialog, NULL);
  /* Settings pages contain editable controls. Restore focus to the instance
   * list so window-level shortcuts such as Ctrl+A are handled by the launcher
   * rather than by the former settings entry. */
  g_idle_add (restore_instance_list_focus, g_object_ref (self));

  if (id != NULL)
    {
      NuvashaLauncherInstance *instance =
        nuvashalauncher_instance_manager_get_by_id (self->model->manager, id);
      gulong handler = (gulong) GPOINTER_TO_SIZE (
        g_object_get_data (G_OBJECT (dialog), "nuvashalauncher-rename-handler"));

      if (instance != NULL && handler != 0)
        g_signal_handler_disconnect (instance, handler);
      g_hash_table_remove (self->model->instance_dialogs, id);
    }
}

/* Keep the instance dialog title synchronized with the instance name. */
static void
on_instance_renamed (NuvashaLauncherInstance *instance, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  const char *id = nuvashalauncher_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  char *name = NULL;

  if (id == NULL)
    return;

  dialog = dialog_of_instance (self, id);

  if (dialog == NULL)
    return;

  g_object_get (instance, "name", &name, NULL);
  adw_dialog_set_title (dialog, name != NULL ? name : "");
  g_free (name);
}

/* Opens @instance: the dialog it already has, brought forward, or a new
 * one over the launcher. The page's header bar carries the view switcher
 * and, because it is inside an AdwDialog, the close button of the
 * dialog. */
void
open_instance_dialog (NuvashaLauncherMainWindow *self, NuvashaLauncherInstance *instance)
{
  const char *id = nuvashalauncher_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  NuvashaLauncherInstancePage *page = NULL;
  char *name = NULL;

  if (id == NULL)
    return;

  dialog = dialog_of_instance (self, id);

  if (dialog != NULL)
    {
      adw_dialog_present (dialog, GTK_WIDGET (self));
      return;
    }

  page = nuvashalauncher_instance_page_new (self->model->manager, instance,
                                    self->model->settings,
                                    window_launcher (self, id));

  g_object_get (instance, "name", &name, NULL);

  dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, name != NULL ? name : "");
  /* The size the separate window used to have, so the lists inside have
     the room they had. */
  adw_dialog_set_content_width (dialog, 900);
  adw_dialog_set_content_height (dialog, 620);
  adw_dialog_set_child (dialog, GTK_WIDGET (page));
  g_free (name);

  /* Store the instance ID so the dialog can be found after rebuilds. */
  g_object_set_data_full (G_OBJECT (dialog), "nuvashalauncher-instance-id",
                          g_strdup (id), g_free);
  g_hash_table_insert (self->model->instance_dialogs, g_strdup (id), dialog);
  /* Weak, so that a dialog that outlives the window cannot call back
     into a window that is gone. */
  g_signal_connect_object (dialog, "closed",
                           G_CALLBACK (on_instance_dialog_closed), self, 0);

  /* The connection goes with the dialog: a page that goes away first
     leaves nothing to call back into. */
  {
     gulong handler = g_signal_connect_object (
       instance, "notify::name", G_CALLBACK (on_instance_renamed), self, 0);
    g_object_set_data (G_OBJECT (dialog), "nuvashalauncher-rename-handler",
                       GSIZE_TO_POINTER ((gsize) handler));
  }

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

void
nuvashalauncher_main_window_show_crash_logs (NuvashaLauncherMainWindow *self,
                                     NuvashaLauncher *launcher)
{
  NuvashaLauncherInstance *instance;
  const char *id;
  AdwDialog *dialog;
  GtkWidget *child;

  g_return_if_fail (NUVASHALAUNCHER_IS_MAIN_WINDOW (self));
  g_return_if_fail (NUVASHALAUNCHER_IS_LAUNCHER (launcher));

  instance = nuvashalauncher_launcher_get_instance (launcher);
  id = instance != NULL ? nuvashalauncher_instance_get_id (instance) : NULL;
  if (instance == NULL || id == NULL)
    return;

  open_instance_dialog (self, instance);
  dialog = dialog_of_instance (self, id);
  if (dialog == NULL)
    return;

  child = adw_dialog_get_child (dialog);
  if (child != NULL && NUVASHALAUNCHER_IS_INSTANCE_PAGE (child))
    nuvashalauncher_instance_page_show_logs (NUVASHALAUNCHER_INSTANCE_PAGE (child));
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* Close dialogs for removed instances. Running launchers remain owned by the
 * window's launcher table. */
static void
close_dialogs_of_removed_instances (NuvashaLauncherMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_autoptr (GPtrArray) doomed = g_ptr_array_new_with_free_func (
    g_object_unref);

  /* Collect first: closing a dialog takes it out of the table being
     walked. */
  g_hash_table_iter_init (&iter, self->model->instance_dialogs);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *id = key;
      AdwDialog *dialog = ADW_DIALOG (value);

      if (nuvashalauncher_instance_manager_get_by_id (self->model->manager, id) == NULL)
        g_ptr_array_add (doomed, g_object_ref (dialog));
    }

  for (guint i = 0; i < doomed->len; i++)
    adw_dialog_force_close (g_ptr_array_index (doomed, i));
}

/* A row clicked anywhere outside its Play button opens the instance dialog. */
void
on_row_activate (GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row,
                 gpointer user_data)
{
  NuvashaLauncherMainWindow *self = NUVASHALAUNCHER_MAIN_WINDOW (user_data);
  GListStore *instances = nuvashalauncher_instance_manager_get_instances (self->model->manager);
  g_autoptr (NuvashaLauncherInstance) instance =
    g_list_model_get_item (G_LIST_MODEL (instances),
                           (guint) gtk_list_box_row_get_index (row));

  if (instance == NULL)
    return;

  open_instance_dialog (self, instance);
}

void
update_visible_page (NuvashaLauncherMainWindow *self)
{
  GListStore *instances = nuvashalauncher_instance_manager_get_instances (self->model->manager);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (instances));

  adw_view_stack_set_visible_child_name (self->ui.stack,
                                         n_items > 0 ? "instances" : "empty");
}

/* An instance that is gone leaves its launcher behind: a game running
 * for it is asked to stop, because there is nothing left to run. The
 * table holds one entry per instance, so walking it is cheap. */
static void
drop_launchers_of_removed_instances (NuvashaLauncherMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;

  g_hash_table_iter_init (&iter, self->model->launchers);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      NuvashaLauncher *launcher = NUVASHALAUNCHER_LAUNCHER (value);

      if (nuvashalauncher_instance_manager_get_by_id (self->model->manager, (const char *) key)
            != NULL)
        continue;

      nuvashalauncher_launcher_stop (launcher);
      g_hash_table_iter_remove (&iter);
    }
}

void
on_instances_changed (NuvashaLauncherMainWindow *self,
                      GListModel    *model G_GNUC_UNUSED,
                      guint          position G_GNUC_UNUSED,
                      guint          removed G_GNUC_UNUSED,
                      guint          added G_GNUC_UNUSED)
{
  update_visible_page (self);
  drop_launchers_of_removed_instances (self);

  /* An instance that is gone has nothing to show, so its dialog goes
     with it. This is how deleting an instance closes its dialog,
     whichever screen the delete was asked from. */
  close_dialogs_of_removed_instances (self);
}
