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
dialog_of_instance (nuvashalauncherMainWindow *self, const char *id)
{
  if (id == NULL)
    return NULL;

  return g_hash_table_lookup (self->model->instance_dialogs, id);
}

static gboolean
restore_instance_list_focus (gpointer user_data)
{
  nuvashalauncherMainWindow *self = user_data;

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
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
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
      nuvashalauncherInstance *instance =
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
on_instance_renamed (nuvashalauncherInstance *instance, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);
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
static void
open_instance_dialog (nuvashalauncherMainWindow *self, nuvashalauncherInstance *instance)
{
  const char *id = nuvashalauncher_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  nuvashalauncherInstancePage *page = NULL;
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
nuvashalauncher_main_window_show_crash_logs (nuvashalauncherMainWindow *self,
                                     nuvashalauncher *launcher)
{
  nuvashalauncherInstance *instance;
  const char *id;
  AdwDialog *dialog;
  GtkWidget *child;

  g_return_if_fail (nuvashalauncher_IS_MAIN_WINDOW (self));
  g_return_if_fail (nuvashalauncher_IS_LAUNCHER (launcher));

  instance = nuvashalauncher_launcher_get_instance (launcher);
  id = instance != NULL ? nuvashalauncher_instance_get_id (instance) : NULL;
  if (instance == NULL || id == NULL)
    return;

  open_instance_dialog (self, instance);
  dialog = dialog_of_instance (self, id);
  if (dialog == NULL)
    return;

  child = adw_dialog_get_child (dialog);
  if (child != NULL && nuvashalauncher_IS_INSTANCE_PAGE (child))
    nuvashalauncher_instance_page_show_logs (nuvashalauncher_INSTANCE_PAGE (child));
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* Close dialogs for removed instances. Running launchers remain owned by the
 * window's launcher table. */
static void
close_dialogs_of_removed_instances (nuvashalauncherMainWindow *self)
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

void
update_instance_selection_ui (nuvashalauncherMainWindow *self)
{
  guint selected = g_hash_table_size (self->ui.selected_instances);
  guint total = g_list_model_get_n_items (
    G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (self->model->manager)));

  if (self->ui.selection_label != NULL)
    {
      g_autofree char *text = g_strdup_printf (_("%u/%u selected"), selected, total);
      gtk_label_set_text (self->ui.selection_label, text);
    }

  if (self->ui.selection_footer != NULL)
    gtk_action_bar_set_revealed (self->ui.selection_footer,
                                 self->ui.edit_mode && selected > 0);

  if (self->ui.footer_delete != NULL)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (self->ui.footer_delete), selected > 0);
      gtk_widget_set_visible (GTK_WIDGET (self->ui.footer_delete),
                              self->ui.edit_mode && selected > 0);
    }
  if (self->ui.footer_export != NULL)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (self->ui.footer_export), selected > 0);
      gtk_widget_set_visible (GTK_WIDGET (self->ui.footer_export),
                              self->ui.edit_mode && selected > 0);
    }
}

void
set_instance_edit_mode (nuvashalauncherMainWindow *self)
{
  self->ui.edit_mode = !self->ui.edit_mode;
  if (!self->ui.edit_mode)
    g_hash_table_remove_all (self->ui.selected_instances);

  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (self->ui.instances));
       row != NULL; row = gtk_widget_get_next_sibling (row))
    {
      GtkWidget *play = g_object_get_data (G_OBJECT (row), "nuvashalauncher-play");
      GtkWidget *check = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvashalauncher-check") : NULL;
      GtkWidget *icon = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvashalauncher-instance-icon") : NULL;
      GtkWidget *edit_actions = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvashalauncher-edit-actions") : NULL;
      GtkWidget *drag_handle = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvashalauncher-drag-handle") : NULL;

      if (play == NULL)
        continue;
      gtk_widget_set_visible (play, !self->ui.edit_mode);
      gtk_widget_set_visible (check, self->ui.edit_mode);
      gtk_widget_set_visible (icon, !self->ui.edit_mode);
      gtk_widget_set_visible (edit_actions, self->ui.edit_mode);
      gtk_widget_set_visible (drag_handle, self->ui.edit_mode);
      if (check != NULL)
        gtk_check_button_set_active (
          GTK_CHECK_BUTTON (check),
          self->ui.selected_instances != NULL &&
          g_object_get_data (G_OBJECT (check), "nuvashalauncher-instance-id") != NULL &&
          g_hash_table_contains (self->ui.selected_instances,
                                 g_object_get_data (G_OBJECT (check),
                                                    "nuvashalauncher-instance-id")));
    }

  update_instance_selection_ui (self);
}

static void
on_bulk_duplicate_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  nuvashalauncherMainWindow *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) copy =
    nuvashalauncher_instance_manager_duplicate_finish (
      nuvashalauncher_INSTANCE_MANAGER (source), result, &error);

  if (copy == NULL && error != NULL)
    {
      nuvashalauncher_ui_error_banner (self->ui.banner, error);
    }
  g_object_unref (self);
}

void
bulk_duplicate_instances (nuvashalauncherMainWindow *self)
{
  GListStore *instances = nuvashalauncher_instance_manager_get_instances (self->model->manager);

  for (guint i = 0; i < g_list_model_get_n_items (G_LIST_MODEL (instances)); i++)
    {
      g_autoptr (nuvashalauncherInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (instances), i);
      if (nuvashalauncher_instance_get_id (instance) != NULL &&
          g_hash_table_contains (self->ui.selected_instances,
                                 nuvashalauncher_instance_get_id (instance)))
        nuvashalauncher_instance_manager_duplicate_async (self->model->manager, instance,
                                                   on_bulk_duplicate_ready,
                                                   g_object_ref (self));
    }
}

typedef struct {
  nuvashalauncherMainWindow *window;
  nuvashalauncherTaskScope *operation;
} BulkDeleteWait;

static void
bulk_delete_wait_free (BulkDeleteWait *wait)
{
  g_object_unref (wait->window);
  nuvashalauncher_task_scope_unref (wait->operation);
  g_free (wait);
}

static void
on_bulk_delete_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  BulkDeleteWait *wait = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_instance_manager_delete_finish (
        nuvashalauncher_INSTANCE_MANAGER (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      nuvashalauncher_ui_error_banner (wait->window->ui.banner, error);
    }

  bulk_delete_wait_free (wait);
}

static void
on_bulk_delete_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  nuvashalauncherMainWindow *self = user_data;
  const char *response = adw_alert_dialog_choose_finish (
    ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "delete") == 0)
    {
      g_autoptr (GPtrArray) selected = g_ptr_array_new_with_free_func (g_object_unref);
      GHashTableIter iter;
      gpointer key;

      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          nuvashalauncherInstance *instance =
            nuvashalauncher_instance_manager_get_by_id (self->model->manager, key);
          if (instance != NULL)
            g_ptr_array_add (selected, g_object_ref (instance));
        }

      g_hash_table_remove_all (self->ui.selected_instances);
      set_instance_edit_mode (self);
      for (guint i = 0; i < selected->len; i++)
        {
          BulkDeleteWait *wait = g_new0 (BulkDeleteWait, 1);
          wait->window = g_object_ref (self);
          wait->operation = nuvashalauncher_instance_manager_delete_async (
            self->model->manager, g_ptr_array_index (selected, i), TRUE,
            on_bulk_delete_ready, wait);
        }
    }

  g_object_unref (self);
}

static void
on_bulk_export_folder_chosen (GObject *source, GAsyncResult *result,
                              gpointer user_data)
{
  nuvashalauncherMainWindow *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) folder = gtk_file_dialog_select_folder_finish (
    GTK_FILE_DIALOG (source), result, &error);

  if (folder != NULL)
    {
      g_autofree char *directory = g_file_get_path (folder);
      if (directory == NULL)
        {
          nuvashalauncher_ui_error_banner (self->ui.banner, NULL);
          adw_banner_set_title (self->ui.banner, _("The export location is not a local file"));
          g_object_unref (self);
          return;
        }
      g_autoptr (GPtrArray) selected = g_ptr_array_new_with_free_func (g_free);
      GHashTableIter iter;
      gpointer key;

      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        g_ptr_array_add (selected, g_strdup (key));

      g_hash_table_remove_all (self->ui.selected_instances);
      set_instance_edit_mode (self);

      for (guint i = 0; i < selected->len; i++)
        {
          const char *id = g_ptr_array_index (selected, i);
          nuvashalauncherInstance *instance =
            nuvashalauncher_instance_manager_get_by_id (self->model->manager, id);
          if (instance != NULL)
            {
              g_autofree char *name = NULL;
              g_autofree char *filename = NULL;
              g_autofree char *path = NULL;
              nuvashalauncherInstancePage *page;

              g_object_get (instance, "name", &name, NULL);
              filename = g_strdup_printf ("%s.mrpack",
                                          name != NULL ? name : "instance");
              path = g_build_filename (directory, filename, NULL);
              page = nuvashalauncher_instance_page_new (
                self->model->manager, instance, self->model->settings,
                window_launcher (self, id));
              g_object_ref_sink (page);
              nuvashalauncher_instance_page_export_to_path (page, path);
              g_object_unref (page);
            }
        }
    }
  else if (error != NULL &&
           !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
           !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    {
      nuvashalauncher_ui_error_banner (self->ui.banner, error);
    }

  g_object_unref (self);
}

void
bulk_export_instances (nuvashalauncherMainWindow *self)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Choose a Directory"));
  gtk_file_dialog_select_folder (dialog, GTK_WINDOW (self), NULL,
                                 on_bulk_export_folder_chosen,
                                 g_object_ref (self));
}

void
bulk_delete_instances (nuvashalauncherMainWindow *self)
{
  g_autofree char *heading = NULL;
  g_autofree char *body = NULL;
  guint selected = g_hash_table_size (self->ui.selected_instances);

  if (selected > 1)
    {
      GString *names = g_string_new (NULL);
      GHashTableIter iter;
      gpointer key = NULL;

      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          nuvashalauncherInstance *instance =
            nuvashalauncher_instance_manager_get_by_id (self->model->manager, key);
          g_autofree char *name = NULL;

          if (instance != NULL)
            g_object_get (instance, "name", &name, NULL);
          if (name == NULL || *name == '\0')
            name = g_strdup ("Unnamed instance");
          if (names->len > 0)
            g_string_append_c (names, '\n');
          g_string_append (names, name);
        }

      body = g_strdup_printf (_("The following instances will be deleted:\n%s"),
                              names->str);
      g_string_free (names, TRUE);
    }

  if (selected == 1)
    {
      GHashTableIter iter;
      gpointer key = NULL;
      g_hash_table_iter_init (&iter, self->ui.selected_instances);
      if (g_hash_table_iter_next (&iter, &key, NULL))
        {
          nuvashalauncherInstance *instance =
            nuvashalauncher_instance_manager_get_by_id (self->model->manager, key);
          g_autofree char *name = NULL;
          if (instance != NULL)
            g_object_get (instance, "name", &name, NULL);
          heading = g_strdup_printf (_("Delete \"%s\"?"),
                                     name != NULL ? name : "instance");
        }
    }

  if (heading == NULL)
    heading = g_strdup_printf (_("Delete \"%s\"?"), "selected instances");

  /* Bulk deletion is confirmed by the same destructive Adwaita alert style
   * used by the existing instance action. The actual removals remain the
   * manager's asynchronous delete operation. */
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (
    heading, body));
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "delete", _("Delete"));
  adw_alert_dialog_set_response_appearance (alert, "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_bulk_delete_chosen, g_object_ref (self));
}

/* A row clicked anywhere but on its buttons opens the instance. The
 * row's index comes from the boxed list, which holds the rows in the
 * same order as the manager. */
void
on_row_activate (GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row,
                 gpointer user_data)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (user_data);

  if (self->ui.edit_mode)
    {
      GtkWidget *play = g_object_get_data (G_OBJECT (row),
                                           "nuvashalauncher-play");
      GtkWidget *check = play != NULL
        ? g_object_get_data (G_OBJECT (play), "nuvashalauncher-check") : NULL;

      if (check != NULL)
        {
          gtk_check_button_set_active (
            GTK_CHECK_BUTTON (check),
            !gtk_check_button_get_active (GTK_CHECK_BUTTON (check)));
          return;
        }
    }

  GListStore *instances = nuvashalauncher_instance_manager_get_instances (self->model->manager);
  nuvashalauncherInstance *instance =
    g_list_model_get_item (G_LIST_MODEL (instances),
                           gtk_list_box_row_get_index (row));

  if (instance == NULL)
    return;

  open_instance_dialog (self, instance);
  g_object_unref (instance);
}

void
update_visible_page (nuvashalauncherMainWindow *self)
{
  GListStore *instances = nuvashalauncher_instance_manager_get_instances (self->model->manager);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (instances));

  if (self->ui.edit != NULL)
    {
      if (n_items == 0 && self->ui.edit_mode)
        set_instance_edit_mode (self);
      gtk_widget_set_visible (GTK_WIDGET (self->ui.edit), n_items > 0);
    }

  adw_view_stack_set_visible_child_name (self->ui.stack,
                                         n_items > 0 ? "instances" : "empty");
}

/* An instance that is gone leaves its launcher behind: a game running
 * for it is asked to stop, because there is nothing left to run. The
 * table holds one entry per instance, so walking it is cheap. */
static void
drop_launchers_of_removed_instances (nuvashalauncherMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;

  g_hash_table_iter_init (&iter, self->model->launchers);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      nuvashalauncher *launcher = nuvashalauncher_LAUNCHER (value);

      if (nuvashalauncher_instance_manager_get_by_id (self->model->manager, (const char *) key)
            != NULL)
        continue;

      nuvashalauncher_launcher_stop (launcher);
      g_hash_table_iter_remove (&iter);
    }
}

void
on_instances_changed (nuvashalauncherMainWindow *self,
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
  update_instance_selection_ui (self);
}
