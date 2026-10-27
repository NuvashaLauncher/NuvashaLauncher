/* Recursive tree operations. */
#define _GNU_SOURCE

#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-task-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Remove a tree relative to an already-open parent directory. All directory
 * traversal is descriptor-relative and uses O_NOFOLLOW, so a pathname race
 * cannot turn cleanup into a walk through a symlink. Errors are
 * best-effort here: callers use this helper for cleanup and do not expect a
 * missing path to make the original operation fail. */
static gboolean
remove_tree_at (int parent_fd, const char *name, GError **error)
{
  struct stat info;
  int fd = -1;

  if (fstatat (parent_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
    {
      if (errno == ENOENT)
        return TRUE;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot inspect \"%s\": %s", name, g_strerror (errno));
      return FALSE;
    }

  if (!S_ISDIR (info.st_mode) || S_ISLNK (info.st_mode))
    {
      if (unlinkat (parent_fd, name, 0) != 0 && errno != ENOENT)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "cannot remove \"%s\": %s", name, g_strerror (errno));
          return FALSE;
        }
      return TRUE;
    }

  fd = openat (parent_fd, name,
               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot open \"%s\": %s", name, g_strerror (errno));
      return FALSE;
    }

  {
    DIR *directory = fdopendir (fd);

    if (directory == NULL)
      {
        close (fd);
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "cannot read \"%s\": %s", name, g_strerror (errno));
        return FALSE;
      }

    gboolean success = TRUE;

    errno = 0;
    for (struct dirent *entry = readdir (directory);
         entry != NULL; entry = readdir (directory))
      {
        if (g_str_equal (entry->d_name, ".") ||
            g_str_equal (entry->d_name, ".."))
          continue;

        if (!remove_tree_at (dirfd (directory), entry->d_name, error))
          {
            success = FALSE;
            break;
          }
      }

    if (errno != 0 && (error == NULL || *error == NULL))
      {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "cannot read \"%s\": %s", name, g_strerror (errno));
        success = FALSE;
      }

    closedir (directory);

    if (!success)
      return FALSE;
  }

  /* AT_REMOVEDIR never follows a symlink. If the directory name was replaced
   * while it was being cleaned, this fails rather than deleting a new tree. */
  if (unlinkat (parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot remove directory \"%s\": %s", name,
                   g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

static gboolean
remove_tree_strict (const char *path, GError **error)
{
  g_autofree char *parent = NULL;
  g_autofree char *name = NULL;
  int parent_fd = -1;
  gboolean result;

  if (path == NULL || !g_path_is_absolute (path))
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                           "cannot remove a non-absolute path");
      return FALSE;
    }

  parent = g_path_get_dirname (path);
  name = g_path_get_basename (path);
  parent_fd = nuvashalauncher_fs_open_directory_nofollow (parent, error);
  if (parent_fd < 0)
    return FALSE;

  result = remove_tree_at (parent_fd, name, error);
  close (parent_fd);
  return result;
}

void
nuvashalauncher_fs_remove_tree (const char *path)
{
  g_autofree char *parent = NULL;
  g_autofree char *name = NULL;
  int parent_fd = -1;

  if (path == NULL || !g_path_is_absolute (path))
    return;

  parent = g_path_get_dirname (path);
  name = g_path_get_basename (path);
  parent_fd = nuvashalauncher_fs_open_directory_nofollow (parent, NULL);
  if (parent_fd >= 0)
    {
      (void) remove_tree_at (parent_fd, name, NULL);
      close (parent_fd);
    }
}

typedef struct {
  char *path;
  nuvashalauncherTaskScope *scope;
} RemoveTreeData;

static void
remove_tree_data_free (RemoveTreeData *data)
{
  g_free (data->path);
  g_free (data);
}

static void
remove_tree_thread (GTask *task G_GNUC_UNUSED,
                    gpointer source_object G_GNUC_UNUSED,
                    gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
  RemoveTreeData *data = task_data;
  nuvashalauncherTaskScope *scope = data->scope;

  if (nuvashalauncher_task_scope_return_error_if_cancelled (scope))
    return;

  g_autoptr (GError) error = NULL;

  if (!remove_tree_strict (data->path, &error))
    {
      nuvashalauncher_task_scope_return_error (scope, g_steal_pointer (&error));
      return;
    }

  if (!nuvashalauncher_task_scope_return_error_if_cancelled (scope))
    nuvashalauncher_task_scope_return_boolean (scope, TRUE);
}

nuvashalauncherTaskScope *
nuvashalauncher_fs_remove_tree_async (const char *path, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvashalauncher_task_scope_task (scope);
  RemoveTreeData *data = g_new0 (RemoveTreeData, 1);

  data->path = g_strdup (path);
  data->scope = scope;
  g_task_set_task_data (task, data,
                        (GDestroyNotify) remove_tree_data_free);
  g_task_run_in_thread (task, remove_tree_thread);
  return g_steal_pointer (&scope);
}

gboolean
nuvashalauncher_fs_remove_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

typedef struct {
  GFile *source;
  GFile *destination;
  nuvashalauncherTaskScope *scope;
} CopyTreeData;

static void
copy_tree_data_free (CopyTreeData *data)
{
  g_clear_object (&data->source);
  g_clear_object (&data->destination);
  g_free (data);
}

static gboolean
copy_tree_sync (GFile *source, GFile *destination, GCancellable *cancellable,
                GError **error)
{
  g_autoptr (GFileInfo) info = NULL;
  GFileType type;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  info = g_file_query_info (source,
                            G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                            G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            cancellable, error);
  if (info == NULL)
    return FALSE;

  type = g_file_info_get_file_type (info);
  if (type == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr (GFileEnumerator) enumerator = NULL;
      GFileInfo *child_info = NULL;

      if (!g_file_make_directory (destination, cancellable, error) &&
          !g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        return FALSE;
      g_clear_error (error);

      enumerator = g_file_enumerate_children (
        source, G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (enumerator == NULL)
        return FALSE;

      while ((child_info = g_file_enumerator_next_file (enumerator,
                                                         cancellable, error)) != NULL)
        {
          g_autoptr (GFile) child_source = g_file_get_child (
            source, g_file_info_get_name (child_info));
          g_autoptr (GFile) child_destination = g_file_get_child (
            destination, g_file_info_get_name (child_info));

          if (!copy_tree_sync (child_source, child_destination, cancellable,
                               error))
            {
              g_object_unref (child_info);
              return FALSE;
            }
          g_object_unref (child_info);
        }

      return *error == NULL;
    }

  if (type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      const char *target = g_file_info_get_symlink_target (info);
      g_autofree char *destination_path = g_file_get_path (destination);

      if (target == NULL || destination_path == NULL ||
          symlink (target, destination_path) != 0)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "cannot copy symlink: %s", g_strerror (errno));
          return FALSE;
        }
      return TRUE;
    }

  return g_file_copy (source, destination, G_FILE_COPY_NONE, cancellable,
                      NULL, NULL, error);
}

static void
copy_tree_thread (GTask *task G_GNUC_UNUSED,
                   gpointer source_object G_GNUC_UNUSED,
                  gpointer task_data, GCancellable *cancellable)
{
  CopyTreeData *data = task_data;
  nuvashalauncherTaskScope *scope = data->scope;
  g_autoptr (GError) error = NULL;

  if (copy_tree_sync (data->source, data->destination, cancellable,
                      &error))
    nuvashalauncher_task_scope_return_boolean (scope, TRUE);
  else
    nuvashalauncher_task_scope_return_error (scope, g_steal_pointer (&error));
}

nuvashalauncherTaskScope *
nuvashalauncher_fs_copy_tree_async (GFile *source, GFile *destination,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvashalauncher_task_scope_task (scope);
  CopyTreeData *data = g_new0 (CopyTreeData, 1);

  data->source = g_object_ref (source);
  data->destination = g_object_ref (destination);
  data->scope = scope;
  g_task_set_task_data (task, data, (GDestroyNotify) copy_tree_data_free);
  g_task_run_in_thread (task, copy_tree_thread);
  return g_steal_pointer (&scope);
}

gboolean
nuvashalauncher_fs_copy_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}
