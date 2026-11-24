/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Async Java discovery, custom-JDK probing, and file-picker handling for
 * NuvashaLauncherJavaRow. */

#include "nuvashalauncher-java-row-private.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-fs.h"

#include <sys/stat.h>

void
nuvashalauncher_java_row_detect_finished (GObject      *source G_GNUC_UNUSED,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
  JavaAsk *ask = user_data;
  NuvashaLauncherJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = nuvashalauncher_java_detect_finish (result, &error);

  /* A detection of a row that has been reloaded since is about a machine
     state that is gone: it fills nothing. */
  if (self != NULL && self->operation == ask->operation && !self->disposed)
    {
      if (javas != NULL)
        {
          g_clear_pointer (&self->javas, g_ptr_array_unref);
          self->javas = g_steal_pointer (&javas);
          nuvashalauncher_java_row_fill (self);
        }
      else if (error != NULL &&
               !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          nuvashalauncher_java_row_show_error (self, nuvashalauncher_error_message (error));
          gtk_widget_set_tooltip_text (GTK_WIDGET (self),
                                       nuvashalauncher_error_message (error));
        }
    }

  nuvashalauncher_java_row_ask_free (ask);
}

void
nuvashalauncher_java_row_custom_loaded (GObject      *source G_GNUC_UNUSED,
                                        GAsyncResult *result,
                                        gpointer      user_data)
{
  JavaAsk *ask = user_data;
  NuvashaLauncherJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  NuvashaLauncherJava *custom = nuvashalauncher_java_probe_finish (result, &error);

  /* Disposing or reloading the row cancels a custom probe. Do not rebuild
   * a row whose state has already been torn down for that cancellation. */
  if (self == NULL || self->disposed || self->operation != ask->operation ||
      (error != NULL &&
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
    {
      nuvashalauncher_java_free (custom);
      nuvashalauncher_java_row_ask_free (ask);
      return;
    }

  if (custom != NULL)
    {
      nuvashalauncher_java_register_custom_path (custom->executable);
      nuvashalauncher_java_free (self->probed);
      self->probed = custom;
    }
  else
    {
      /* Probing failed because the configured runtime was removed or is
       * no longer usable: clear the selection back to automatic. */
      nuvashalauncher_java_free (self->probed);
      self->probed = NULL;
      g_clear_pointer (&self->wanted, g_free);
    }

  nuvashalauncher_java_row_fill (self);
  /* Reloading a configured custom runtime can keep the same numeric combo
   * selection, so notify the owner even when GTK suppresses a selected-value
   * change. The instance page must revalidate its status subtitle. */
  g_object_notify (G_OBJECT (self), "selected");
  nuvashalauncher_java_row_ask_free (ask);
}

void
nuvashalauncher_java_row_on_retry_clicked (GtkButton *button G_GNUC_UNUSED,
                                           gpointer   user_data)
{
  NuvashaLauncherJavaRow *self = NUVASHALAUNCHER_JAVA_ROW (user_data);

  nuvashalauncher_java_row_load (self, self->wanted);
}

/* ---------------------------------------------------------- *
 * Adding a runtime the scan did not report
 *
 * The last entry of the list asks for one instead of naming one, so it
 * carries no path and the caller's handler has nothing to store. Picking
 * it puts the row back to the entry it came from, so the list never
 * shows the ask as the answer, and opens the chooser. What comes back is
 * probed and appended like any other runtime, and selected: the selection
 * is what tells the caller which Java the user wants.
 * ---------------------------------------------------------- */

typedef struct
{
  NuvashaLauncherJavaRow *row;
  NuvashaLauncherTaskScope *operation;
  char *dest_home;
  char *rel_exec;
} JavaCopyTask;

static void probe_custom_path (NuvashaLauncherJavaRow *self, const char *path);

static void
on_java_copy_finished (GObject      *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  JavaCopyTask *task = user_data;
  NuvashaLauncherJavaRow *self = task->row;
  g_autoptr (GError) error = NULL;
  gboolean ok = nuvashalauncher_fs_copy_tree_finish (result, &error);

  if (self == NULL || self->disposed || self->operation != task->operation)
    {
      if (!ok)
        nuvashalauncher_fs_remove_tree (task->dest_home);
      g_clear_object (&task->row);
      if (task->operation != NULL)
        nuvashalauncher_task_scope_unref (task->operation);
      g_free (task->dest_home);
      g_free (task->rel_exec);
      g_free (task);
      return;
    }

  if (!ok)
    {
      nuvashalauncher_fs_remove_tree (task->dest_home);
      adw_action_row_set_subtitle (
        ADW_ACTION_ROW (self),
        error != NULL ? nuvashalauncher_error_message (error)
                      : _("The Java executable could not be used. Select a working Java runtime."));
      gtk_widget_set_tooltip_text (GTK_WIDGET (self),
                                   error != NULL ? nuvashalauncher_error_message (error) : NULL);
      g_clear_object (&task->row);
      if (task->operation != NULL)
        nuvashalauncher_task_scope_unref (task->operation);
      g_free (task->dest_home);
      g_free (task->rel_exec);
      g_free (task);
      return;
    }

  {
    g_autofree char *new_executable = g_build_filename (task->dest_home, task->rel_exec, NULL);
    chmod (new_executable, 0755);
    probe_custom_path (self, new_executable);
  }

  g_clear_object (&task->row);
  if (task->operation != NULL)
    nuvashalauncher_task_scope_unref (task->operation);
  g_free (task->dest_home);
  g_free (task->rel_exec);
  g_free (task);
}

static void
add_probed (GObject      *source G_GNUC_UNUSED,
            GAsyncResult *result,
            gpointer      user_data)
{
  JavaAsk *ask = user_data;
  NuvashaLauncherJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  NuvashaLauncherJava *probe = nuvashalauncher_java_probe_finish (result, &error);

  if (self == NULL || self->disposed || self->operation != ask->operation)
    {
      nuvashalauncher_java_free (probe);
      nuvashalauncher_java_row_ask_free (ask);
      return;
    }

  if (probe == NULL)
    {
      adw_action_row_set_subtitle (
        ADW_ACTION_ROW (self),
        error != NULL ? nuvashalauncher_error_message (error)
                      : _("The Java executable could not be used. Select a working Java runtime."));
      gtk_widget_set_tooltip_text (GTK_WIDGET (self),
                                   error != NULL ? nuvashalauncher_error_message (error) : NULL);
      nuvashalauncher_java_row_ask_free (ask);
      return;
    }

  {
    g_autofree char *source_home = NULL;
    g_autofree char *rel_exec = NULL;

    if (nuvashalauncher_java_should_copy (probe->executable, &source_home, &rel_exec))
      {
        g_autofree char *base_name = g_path_get_basename (source_home);
        g_autofree char *launcher_dir = nuvashalauncher_java_get_dir ();
        g_autofree char *safe_name = NULL;
        char *dest_home = NULL;
        guint counter = 1;

        if (base_name == NULL || *base_name == '\0' ||
            g_strcmp0 (base_name, ".") == 0 || g_strcmp0 (base_name, "/") == 0)
          safe_name = g_strdup_printf ("java-%d", probe->major_version > 0 ? probe->major_version : 21);
        else
          safe_name = g_strdup (base_name);

        dest_home = g_build_filename (launcher_dir, safe_name, NULL);

        while (g_file_test (dest_home, G_FILE_TEST_EXISTS))
          {
            g_autofree char *real_src = realpath (source_home, NULL);
            g_autofree char *real_dst = realpath (dest_home, NULL);

            if (real_src != NULL && real_dst != NULL && g_strcmp0 (real_src, real_dst) == 0)
              break;

            g_free (dest_home);
            dest_home = g_strdup_printf ("%s/%s-%u", launcher_dir, safe_name, counter++);
          }

        {
          g_autofree char *real_src = realpath (source_home, NULL);
          g_autofree char *real_dst = realpath (dest_home, NULL);

          if (real_src != NULL && real_dst != NULL && g_strcmp0 (real_src, real_dst) == 0)
            {
              g_free (dest_home);
              const char *executable = probe->executable;
              nuvashalauncher_java_register_custom_path (executable);
              nuvashalauncher_java_row_set_custom (self, executable, g_steal_pointer (&probe));
              g_object_notify (G_OBJECT (self), "selected");
              nuvashalauncher_java_row_ask_free (ask);
              return;
            }
        }

        adw_action_row_set_subtitle (ADW_ACTION_ROW (self), _("Loading…"));
        gtk_widget_set_tooltip_text (GTK_WIDGET (self), NULL);

        JavaCopyTask *copy_task = g_new0 (JavaCopyTask, 1);
        copy_task->row = g_object_ref (self);
        copy_task->dest_home = dest_home;
        copy_task->rel_exec = g_steal_pointer (&rel_exec);

        g_autoptr (GFile) src_file = g_file_new_for_path (source_home);
        g_autoptr (GFile) dst_file = g_file_new_for_path (dest_home);

        nuvashalauncher_java_free (probe);
        nuvashalauncher_java_row_ask_free (ask);

        copy_task->operation = nuvashalauncher_fs_copy_tree_async (
          src_file, dst_file, on_java_copy_finished, copy_task);
        nuvashalauncher_task_scope_follow_cancellable (copy_task->operation, self->cancellable);
        self->operation = copy_task->operation;
        return;
      }
  }

  {
    const char *executable = probe->executable;

    nuvashalauncher_java_register_custom_path (executable);
    nuvashalauncher_java_row_set_custom (self, executable, g_steal_pointer (&probe));
    /* A custom runtime can occupy the same numeric selection as the
     * placeholder it replaces. GTK then quite correctly suppresses
     * notify::selected, but the owning instance still needs to persist the
     * newly probed executable. */
    g_object_notify (G_OBJECT (self), "selected");
  }

  nuvashalauncher_java_row_ask_free (ask);
}

static void
probe_custom_path (NuvashaLauncherJavaRow *self, const char *path)
{
  JavaAsk *ask = g_new0 (JavaAsk, 1);

  if (self->operation != NULL)
    nuvashalauncher_task_scope_cancel (self->operation);

  ask->row = g_object_ref (self);
  ask->operation = nuvashalauncher_java_probe_async (path, add_probed, ask);
  nuvashalauncher_task_scope_follow_cancellable (ask->operation, self->cancellable);
  self->operation = ask->operation;
}

static void
add_chosen (GObject *dialog, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncherJavaRow *self = NUVASHALAUNCHER_JAVA_ROW (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_open_finish (GTK_FILE_DIALOG (dialog), result, &error);

  g_object_unref (dialog);

  if (self->disposed)
    {
      g_object_unref (self);
      return;
    }

  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED))
        {
          adw_action_row_set_subtitle (ADW_ACTION_ROW (self),
                                       nuvashalauncher_error_message (error));
          gtk_widget_set_tooltip_text (GTK_WIDGET (self),
                                       nuvashalauncher_error_message (error));
        }

      g_object_unref (self);
      return;
    }

  {
    g_autofree char *path = g_file_get_path (file);

    if (path == NULL)
      adw_action_row_set_subtitle (
        ADW_ACTION_ROW (self),
        _("the chosen file does not report a Java version"));
    else
      {
        g_autofree char *resolved_exec = NULL;

        if (!g_str_has_suffix (path, "/java") && g_strcmp0 (g_path_get_basename (path), "java") != 0)
          {
            g_autofree char *dir = g_file_test (path, G_FILE_TEST_IS_DIR) ? g_strdup (path) : g_path_get_dirname (path);
            g_autofree char *bin_java = g_build_filename (dir, "bin", "java", NULL);
            if (g_file_test (bin_java, G_FILE_TEST_IS_REGULAR))
              resolved_exec = g_steal_pointer (&bin_java);
          }

        probe_custom_path (self, resolved_exec != NULL ? resolved_exec : path);
      }
  }

  g_object_unref (self);
}

static void
open_add_dialog (NuvashaLauncherJavaRow *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());

  gtk_file_dialog_set_initial_folder (dialog, home);
  gtk_file_dialog_set_title (dialog, _("Choose Java executable"));
  gtk_file_dialog_open (dialog, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, add_chosen, g_object_ref (self));
}

void
nuvashalauncher_java_row_on_add_clicked (GtkButton *button G_GNUC_UNUSED,
                                         gpointer   user_data)
{
  open_add_dialog (NUVASHALAUNCHER_JAVA_ROW (user_data));
}

void
nuvashalauncher_java_row_on_picked (GObject    *object G_GNUC_UNUSED,
                                    GParamSpec *pspec G_GNUC_UNUSED,
                                    gpointer    user_data)
{
  NuvashaLauncherJavaRow *self = NUVASHALAUNCHER_JAVA_ROW (user_data);
  guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (self));

  if (self->rebuilding)
    return;

  if (selected == self->add_index)
    {
      adw_combo_row_set_selected (ADW_COMBO_ROW (self), self->restore);
      open_add_dialog (self);
    }
  else if (selected < self->ids->len)
    {
      self->restore = selected;
      self->automatic_selection = self->ids->pdata[selected] == NULL;
      g_set_str (&self->wanted, self->ids->pdata[selected]);
    }
}
