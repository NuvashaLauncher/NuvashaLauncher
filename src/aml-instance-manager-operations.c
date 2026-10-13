/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Asynchronous instance directory operations. Metadata and validation stay
 * in aml-instance-manager.c. */

#include "aml-instance-manager-private.h"
#include "aml-fs.h"
#include "aml-i18n.h"
#include "aml-task-private.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Duplicate
 * ---------------------------------------------------------- */

typedef struct
{
  AmlInstanceManager *self;
  GTask *task;
  AmlTaskScope *scope;
  AmlTaskScope *copy_scope;
  AmlInstance *copy; /* built, not registered yet */
} DuplicateData;

static void
duplicate_data_free (DuplicateData *data)
{
  g_clear_object (&data->task);
  aml_task_scope_unref (data->scope);
  if (data->copy_scope != NULL)
    aml_task_scope_unref (data->copy_scope);
  g_clear_object (&data->self);
  g_clear_object (&data->copy);
  g_free (data);
}

static void
duplicate_data_return_error (DuplicateData *data, GError *error)
{
  aml_task_scope_return_error_deferred (data->scope, error);
  duplicate_data_free (data);
}

static void
duplicate_copy_ready (GObject *source G_GNUC_UNUSED,
                      GAsyncResult *result, gpointer user_data)
{
  DuplicateData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!copy_tree_finish (result, &error))
    {
      /* Nothing was registered: the copy stays a plain directory. */
      duplicate_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  g_list_store_append (data->self->instances, data->copy);

  if (!aml_instance_manager_save (data->self, &error))
    {
      guint position = 0;

      /* The file on disk never knew about the copy. */
      if (g_list_store_find (data->self->instances, data->copy, &position))
        g_list_store_remove (data->self->instances, position);

      duplicate_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  aml_task_scope_return_pointer_deferred (data->scope, data->copy,
                                          g_object_unref);
  data->copy = NULL; /* the task hands it to the finish call */
  duplicate_data_free (data);
}

AmlTaskScope *
aml_instance_manager_duplicate_async (AmlInstanceManager *self,
                                      AmlInstance *source_instance,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE (source_instance), NULL);

  g_autoptr (GTask) task = g_task_new (self, NULL, callback, user_data);
  aml_task_scope_attach_task (task);
  AmlTaskScope *task_scope = g_object_get_data (G_OBJECT (task),
                                                 "aml-task-scope");
  if (!g_list_store_find (self->instances, source_instance, NULL))
    {
      aml_task_scope_return_error (
        task_scope,
        g_error_new_literal (AML_INSTANCE_MANAGER_ERROR,
                             AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                             "the instance is not registered"));
       return aml_task_scope_ref (task_scope);
    }

  g_autofree char *source_name = instance_name (source_instance);
  g_autofree char *source_directory = instance_directory (source_instance);
  g_autofree char *minecraft_version = NULL;
  g_autofree char *loader_version = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *java_path = NULL;
  g_autofree char *jvm_args = NULL;
  AmlLoader loader = AML_LOADER_VANILLA;

  g_object_get (source_instance,
                "minecraft_version", &minecraft_version,
                "loader", &loader,
                "loader_version", &loader_version,
                "icon", &icon,
                "java_path", &java_path,
                "jvm_args", &jvm_args,
                NULL);

  g_autofree char *id = g_uuid_string_random ();
   g_autofree char *name = g_strdup_printf (_("Copy of %s"), source_name);
   g_autofree char *unique_name = unique_instance_name (self, name);
   g_autofree char *directory = default_instance_directory (self, unique_name);
  g_autofree char *canonical = NULL;
  g_autoptr (GError) local_error = NULL;

   if (!check_instance_directory (self, directory, NULL, &canonical, &local_error))
     {
       aml_task_scope_return_error (task_scope, g_steal_pointer (&local_error));
       return aml_task_scope_ref (task_scope);
     }

  g_free (directory);
  directory = g_steal_pointer (&canonical);

  g_autoptr (AmlInstance) copy =
     aml_instance_new (id, unique_name, directory, minecraft_version, loader,
                      &local_error);

   if (copy == NULL)
     {
       aml_task_scope_return_error (task_scope, g_steal_pointer (&local_error));
        return aml_task_scope_ref (task_scope);
     }

  g_object_set (copy,
                "loader_version", loader_version,
                "icon", icon,
                "java_path", java_path,
                "jvm_args", jvm_args,
                NULL);

  g_autoptr (GError) instances_error = NULL;
  if (!aml_fs_make_directory (self->instances_dir, 0755,
                              &instances_error))
    {
      aml_task_scope_return_error (
        task_scope,
        g_error_new (AML_INSTANCE_MANAGER_ERROR,
                     AML_INSTANCE_MANAGER_ERROR_IO,
                     "cannot create \"%s\": %s",
                     self->instances_dir, instances_error->message));
        return aml_task_scope_ref (task_scope);
    }

  DuplicateData *data = g_new0 (DuplicateData, 1);
  data->self = g_object_ref (self);
  data->task = g_object_ref (task);
  data->scope = aml_task_scope_ref (task_scope);
  data->copy = g_steal_pointer (&copy);

  g_autoptr (GFile) source_file = g_file_new_for_path (source_directory);
  g_autoptr (GFile) copy_file = g_file_new_for_path (directory);

  data->copy_scope = copy_tree_async (source_file, copy_file,
                                      duplicate_copy_ready, data);
  return aml_task_scope_ref (task_scope);
}

AmlInstance *
aml_instance_manager_duplicate_finish (AmlInstanceManager *self,
                                       GAsyncResult *result,
                                       GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static AmlTaskScope *
delete_tree_async (GFile *file, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path = g_file_get_path (file);

  (void) cancellable;
  return aml_fs_remove_tree_async (path, callback, user_data);
}

static gboolean
delete_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Delete
 * ---------------------------------------------------------- */

typedef struct
{
  AmlInstanceManager *self;
  GTask *task;
  AmlTaskScope *scope;
  AmlTaskScope *tree_scope;
  AmlInstance *instance;
} DeleteData;

static void
delete_data_free (DeleteData *data)
{
  g_clear_object (&data->task);
  aml_task_scope_unref (data->scope);
  if (data->tree_scope != NULL)
    aml_task_scope_unref (data->tree_scope);
  g_clear_object (&data->self);
  g_clear_object (&data->instance);
  g_free (data);
}

static void
delete_data_return_error (DeleteData *data, GError *error)
{
  aml_task_scope_return_error_deferred (data->scope, error);
  delete_data_free (data);
}

static void
delete_directory_ready (GObject *source G_GNUC_UNUSED,
                        GAsyncResult *result, gpointer user_data)
{
  DeleteData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!delete_tree_finish (result, &error))
    {
      /* The directory is still there, so keep the instance. */
      delete_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  if (!remove_instance_and_save (data->self, data->instance, &error))
    {
      delete_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  aml_task_scope_return_boolean_deferred (data->scope, TRUE);
  delete_data_free (data);
}

AmlTaskScope *
aml_instance_manager_delete_async (AmlInstanceManager *self,
                                   AmlInstance *instance,
                                   gboolean delete_directory,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);

  g_autoptr (GTask) task = g_task_new (self, NULL, callback, user_data);
  aml_task_scope_attach_task (task);
  AmlTaskScope *task_scope = g_object_get_data (G_OBJECT (task),
                                                 "aml-task-scope");
  if (!g_list_store_find (self->instances, instance, NULL))
    {
      aml_task_scope_return_error (
        task_scope,
        g_error_new_literal (AML_INSTANCE_MANAGER_ERROR,
                             AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                             "the instance is not registered"));
       return aml_task_scope_ref (task_scope);
    }

  if (!delete_directory)
    {
      g_autoptr (GError) error = NULL;

      if (!remove_instance_and_save (self, instance, &error))
         aml_task_scope_return_error (task_scope, g_steal_pointer (&error));
       else
         aml_task_scope_return_boolean (task_scope, TRUE);

       return aml_task_scope_ref (task_scope);
    }

  g_autofree char *directory = instance_directory (instance);
  g_autoptr (GFile) file = g_file_new_for_path (directory);

  DeleteData *data = g_new0 (DeleteData, 1);
  data->self = g_object_ref (self);
  data->task = g_object_ref (task);
    data->scope = aml_task_scope_ref (task_scope);
  data->instance = g_object_ref (instance);

  data->tree_scope = delete_tree_async (file, NULL, delete_directory_ready,
                                        data);
  return aml_task_scope_ref (task_scope);
}

gboolean
aml_instance_manager_delete_finish (AmlInstanceManager *self,
                                    GAsyncResult *result,
                                    GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Change directory
 *
 * The files are copied to the new place first and the metadata is
 * written only once they are all there, so a move that fails leaves the
 * old directory and the stored path exactly as they were. The old copy is
 * removed at the very end: a directory that outlives the change is a
 * leftover, while a directory that went before the metadata did would be
 * files with nothing pointing at them.
 * ---------------------------------------------------------- */

gboolean
aml_instance_manager_check_new_directory (AmlInstanceManager  *self,
                                          AmlInstance         *instance,
                                          const char          *directory,
                                          char               **canonical,
                                          GError             **error)
{
  g_autofree char *path = NULL;
  g_autofree char *current = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (canonical == NULL || *canonical == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  if (!check_instance_directory (self, directory, instance, &path, error))
    return FALSE;

  g_object_get (instance, "directory", &current, NULL);

  /* The directory it already has is not a change, and asking for it
     would move a directory onto itself. */
  if (g_strcmp0 (path, current) == 0)
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the directory is already \"%s\"", path);
      return FALSE;
    }

  /* An existing directory is not touched: something else put it there
     and the files of this instance must not land on top of it. */
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "\"%s\" already exists", path);
      return FALSE;
    }

  /* The directory the new one hangs off has to be there already. Left to
     find out during the copy, this would be reported as a file that
     cannot be written, which says nothing about why. */
  {
    g_autofree char *parent = g_path_get_dirname (path);

    if (!g_file_test (parent, G_FILE_TEST_IS_DIR))
      {
        g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                     AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                     "the directory \"%s\" does not exist", parent);
        return FALSE;
      }
  }

  if (canonical != NULL)
    *canonical = g_steal_pointer (&path);

  return TRUE;
}

typedef struct
{
  AmlInstanceManager *self;
  GTask *task;
  AmlTaskScope *scope;
  AmlTaskScope *copy_scope;
  AmlTaskScope *tree_scope;
  AmlInstance *instance;

  char *old_directory;
  char *new_directory;
  GFile *destination;
  GError *undo_error; /* the failure the undo is cleaning up after */
} MoveData;

static void
move_data_free (MoveData *data)
{
  g_clear_object (&data->task);
  aml_task_scope_unref (data->scope);
  if (data->copy_scope != NULL)
    aml_task_scope_unref (data->copy_scope);
  if (data->tree_scope != NULL)
    aml_task_scope_unref (data->tree_scope);
  g_clear_object (&data->self);
  g_clear_object (&data->instance);
  g_clear_object (&data->destination);
  g_free (data->old_directory);
  g_free (data->new_directory);
  g_clear_error (&data->undo_error);
  g_free (data);
}

static void
move_return_error (MoveData *data, GError *error)
{
  aml_task_scope_return_error_deferred (data->scope, error);
  move_data_free (data);
}

/* Puts @instance back to the directory it had and drops whatever landed
 * in the new one. Nothing about the old directory is touched: it was
 * never modified. The destination may hold a partial copy, so it is
 * removed recursively, and @error is returned once that is done. */
static void
move_undo_deleted (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  MoveData *data = user_data;
  g_autoptr (GError) ignored = NULL;

  /* Best effort: the error that caused the undo is what the caller
     needs to hear, not a failure to clean up after it. */
  delete_tree_finish (result, &ignored);

  move_return_error (data, g_steal_pointer (&data->undo_error));
}

static void
move_undo (MoveData *data, gboolean restore_instance, GError *error)
{
  if (restore_instance)
    aml_instance_set_directory (data->instance, data->old_directory, NULL);

  data->undo_error = error;

  if (data->destination != NULL)
    data->tree_scope = delete_tree_async (data->destination, NULL,
                                          move_undo_deleted, data);
  else
    move_return_error (data, g_steal_pointer (&data->undo_error));
}

/* The move has already happened and been saved. What is left is a
 * directory that now holds a second copy of files that exist somewhere
 * else; failing to remove it leaves files behind rather than taking any
 * away, so the change stands and the leftover is said out loud. */
static void
move_old_removed (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  MoveData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!delete_tree_finish (result, &error))
    g_warning ("the old directory \"%s\" could not be removed: %s",
               data->old_directory, error->message);

  aml_task_scope_return_boolean_deferred (data->scope, TRUE);
  move_data_free (data);
}

static void
move_copy_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  MoveData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!copy_tree_finish (result, &error))
    {
      move_undo (data, FALSE, g_steal_pointer (&error));
      return;
    }

  /* The files are all in the new place; only now does the stored path
     change, and the file that records it is written whole. */
    aml_instance_set_directory (data->instance, data->new_directory, NULL);

  if (!aml_instance_manager_save (data->self, &error))
    {
      move_undo (data, TRUE, g_steal_pointer (&error));
      return;
    }

  /* The old directory is now a second copy of a directory that exists.
     Taking it away is the last thing done. */
  {
    g_autoptr (GFile) old = g_file_new_for_path (data->old_directory);

    data->tree_scope = delete_tree_async (old, NULL, move_old_removed, data);
  }
}

AmlTaskScope *
aml_instance_manager_change_directory_async (AmlInstanceManager *self,
                                             AmlInstance *instance,
                                             const char *directory,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);

  g_autoptr (GTask) task = g_task_new (self, NULL, callback, user_data);
  aml_task_scope_attach_task (task);
  AmlTaskScope *task_scope = g_object_get_data (G_OBJECT (task),
                                                 "aml-task-scope");
  g_autofree char *path = NULL;
  g_autofree char *old_directory = NULL;
  g_autoptr (GError) local_error = NULL;

  if (!aml_instance_manager_check_new_directory (self, instance, directory,
                                                 &path, &local_error))
    {
       aml_task_scope_return_error (task_scope, g_steal_pointer (&local_error));
       return aml_task_scope_ref (task_scope);
    }

  g_object_get (instance, "directory", &old_directory, NULL);

  MoveData *data = g_new0 (MoveData, 1);
  data->self = g_object_ref (self);
  data->task = g_steal_pointer (&task);
  data->scope = aml_task_scope_ref (task_scope);
  data->instance = g_object_ref (instance);
  data->old_directory = g_steal_pointer (&old_directory);
  data->new_directory = g_strdup (path);
  data->destination = g_file_new_for_path (path);

  /* An instance whose directory is gone has nothing to move: the path is
     changed and saved, which is all that is left to do. */
  if (!g_file_test (data->old_directory, G_FILE_TEST_IS_DIR))
    {
      aml_instance_set_directory (instance, data->new_directory, NULL);

      if (!aml_instance_manager_save (self, &local_error))
        {
          aml_instance_set_directory (instance, data->old_directory, NULL);
          move_return_error (data, g_steal_pointer (&local_error));
          return aml_task_scope_ref (task_scope);
        }

       aml_task_scope_return_boolean_deferred (data->scope, TRUE);
      move_data_free (data);
      return aml_task_scope_ref (task_scope);
    }

  {
    g_autoptr (GFile) source = g_file_new_for_path (data->old_directory);

     data->copy_scope = copy_tree_async (source, data->destination,
                                         move_copy_ready, data);
  }
  return aml_task_scope_ref (task_scope);
}

gboolean
aml_instance_manager_change_directory_finish (AmlInstanceManager *self,
                                               GAsyncResult *result,
                                               GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Open folder
 * ---------------------------------------------------------- */

static void
open_folder_launch_ready (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data; /* holds its own reference */
  AmlTaskScope *scope = g_object_get_data (G_OBJECT (task),
                                           "aml-task-scope");
  g_autoptr (GError) error = NULL;

  if (g_app_info_launch_default_for_uri_finish (result, &error))
    aml_task_scope_return_boolean (scope, TRUE);
  else
    aml_task_scope_return_error (scope, g_steal_pointer (&error));

  g_object_unref (task);
}

AmlTaskScope *
aml_instance_manager_open_folder_async (AmlInstanceManager *self,
                                         AmlInstance *instance,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);

  g_autoptr (GTask) task = g_task_new (self, NULL, callback, user_data);
   aml_task_scope_attach_task (task);
   AmlTaskScope *scope = g_object_get_data (G_OBJECT (task),
                                            "aml-task-scope");

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      aml_task_scope_return_error (
        scope,
        g_error_new_literal (AML_INSTANCE_MANAGER_ERROR,
                             AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                             "the instance is not registered"));
      return aml_task_scope_ref (scope);
    }

  g_autofree char *directory = instance_directory (instance);

  if (!g_file_test (directory, G_FILE_TEST_IS_DIR))
    {
      aml_task_scope_return_error (
        scope,
        g_error_new (AML_INSTANCE_MANAGER_ERROR,
                     AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                     "the directory \"%s\" does not exist", directory));
      return aml_task_scope_ref (scope);
    }

  g_autoptr (GError) error = NULL;
  g_autofree char *uri = g_filename_to_uri (directory, NULL, &error);

  if (uri == NULL)
    {
      aml_task_scope_return_error (scope, g_steal_pointer (&error));
      return aml_task_scope_ref (scope);
    }

  g_app_info_launch_default_for_uri_async (
    uri, NULL, aml_task_scope_get_cancellable (scope),
                                            open_folder_launch_ready,
                                            g_object_ref (task));
  return aml_task_scope_ref (scope);
}

gboolean
aml_instance_manager_open_folder_finish (AmlInstanceManager *self,
                                         GAsyncResult *result,
                                         GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
