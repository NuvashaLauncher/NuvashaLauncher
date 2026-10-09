/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-cache.h"

#define CACHE_SUBDIR "AML"

typedef struct {
  GTask *task;
  GFileEnumerator *enumerator;
} ClearCache;

static char *
cache_directory (const char *child)
{
  return g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, child, NULL);
}

char *
aml_cache_get_api_directory (void)
{
  return cache_directory ("api");
}

char *
aml_cache_get_files_directory (void)
{
  return cache_directory ("files");
}

static void clear_next (ClearCache *clear);

static void
clear_free (ClearCache *clear)
{
  g_clear_object (&clear->enumerator);
  g_clear_object (&clear->task);
  g_free (clear);
}

static void
clear_done (ClearCache *clear, GError *error)
{
  if (error == NULL)
    g_task_return_boolean (clear->task, TRUE);
  else
    g_task_return_error (clear->task, error);
  clear_free (clear);
}

static void
clear_deleted (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ClearCache *clear = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_delete_finish (G_FILE (source), result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&error);
          clear_next (clear);
          return;
        }
      clear_done (clear, g_steal_pointer (&error));
      return;
    }

  clear_next (clear);
}

static void
clear_file_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ClearCache *clear = user_data;
  g_autoptr (GError) error = NULL;
  GList *files = g_file_enumerator_next_files_finish (
    G_FILE_ENUMERATOR (source), result, &error);

  if (files == NULL)
    {
      if (error != NULL)
        clear_done (clear, g_steal_pointer (&error));
      else
        clear_done (clear, NULL);
      return;
    }

  {
    GFileInfo *info = files->data;
    g_autoptr (GFile) file = g_file_get_child (
      g_file_enumerator_get_container (clear->enumerator),
      g_file_info_get_name (info));

    g_list_free_full (files, g_object_unref);
    g_file_delete_async (file, G_PRIORITY_DEFAULT,
                         g_task_get_cancellable (clear->task),
                         clear_deleted, clear);
  }
}

static void
clear_next (ClearCache *clear)
{
  g_file_enumerator_next_files_async (
    clear->enumerator, 1, G_PRIORITY_DEFAULT,
    g_task_get_cancellable (clear->task), clear_file_ready, clear);
}

static void
clear_opened (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ClearCache *clear = user_data;
  g_autoptr (GError) error = NULL;

  clear->enumerator = g_file_enumerate_children_finish (
    G_FILE (source), result, &error);
  if (clear->enumerator == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&error);
          clear_done (clear, NULL);
        }
      else
        clear_done (clear, g_steal_pointer (&error));
      return;
    }

  clear_next (clear);
}

void
aml_cache_clear_api_async (GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  ClearCache *clear = g_new0 (ClearCache, 1);
  g_autofree char *path = aml_cache_get_api_directory ();
  g_autoptr (GFile) directory =
    g_file_new_for_path (path);

  clear->task = g_steal_pointer (&task);
  g_file_enumerate_children_async (
    directory, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE,
    G_PRIORITY_DEFAULT, cancellable, clear_opened, clear);
}

/* ---------------------------------------------------------- *
 * Clearing the complete global cache tree
 * ---------------------------------------------------------- */

typedef struct {
  GFile *directory;
  GFileEnumerator *enumerator;
} ClearFrame;

typedef struct {
  GTask *task;
  GPtrArray *frames; /* ClearFrame*, root first */
} ClearTree;

static void clear_tree_next (ClearTree *tree);

static void
clear_frame_free (ClearFrame *frame)
{
  if (frame == NULL)
    return;
  g_clear_object (&frame->directory);
  g_clear_object (&frame->enumerator);
  g_free (frame);
}

static void
clear_tree_free (ClearTree *tree)
{
  for (guint i = 0; i < tree->frames->len; i++)
    clear_frame_free (g_ptr_array_index (tree->frames, i));
  g_ptr_array_unref (tree->frames);
  g_clear_object (&tree->task);
  g_free (tree);
}

static void
clear_tree_done (ClearTree *tree, GError *error)
{
  if (error == NULL)
    g_task_return_boolean (tree->task, TRUE);
  else
    g_task_return_error (tree->task, error);
  clear_tree_free (tree);
}

static ClearFrame *
clear_tree_current (ClearTree *tree)
{
  return g_ptr_array_index (tree->frames, tree->frames->len - 1);
}

static void
clear_tree_deleted (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  ClearTree *tree = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_delete_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      clear_tree_done (tree, g_steal_pointer (&error));
      return;
    }
  clear_tree_next (tree);
}

static void
clear_tree_child_deleted (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  ClearTree *tree = user_data;
  g_autoptr (GError) error = NULL;
  ClearFrame *child = g_ptr_array_steal_index (tree->frames,
                                                tree->frames->len - 1);

  if (!g_file_delete_finish (G_FILE (source), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      clear_frame_free (child);
      clear_tree_done (tree, g_steal_pointer (&error));
      return;
    }

  clear_frame_free (child);
  clear_tree_next (tree);
}

static void
clear_tree_opened (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ClearTree *tree = user_data;
  ClearFrame *frame = clear_tree_current (tree);
  g_autoptr (GError) error = NULL;

  frame->enumerator = g_file_enumerate_children_finish (
    G_FILE (source), result, &error);
  if (frame->enumerator == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&error);
          clear_tree_done (tree, NULL);
        }
      else
        clear_tree_done (tree, g_steal_pointer (&error));
      return;
    }

  clear_tree_next (tree);
}

static void
clear_tree_file_ready (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  ClearTree *tree = user_data;
  ClearFrame *frame = clear_tree_current (tree);
  g_autoptr (GError) error = NULL;
  GList *items = g_file_enumerator_next_files_finish (
    G_FILE_ENUMERATOR (source), result, &error);

  if (items == NULL)
    {
      if (error != NULL)
        {
          clear_tree_done (tree, g_steal_pointer (&error));
          return;
        }

      if (tree->frames->len == 1)
        {
          clear_tree_done (tree, NULL);
          return;
        }

      /* The current directory is empty; remove it and resume its parent. */
      {
        ClearFrame *child = g_ptr_array_index (tree->frames,
                                               tree->frames->len - 1);
        g_file_delete_async (child->directory, G_PRIORITY_DEFAULT,
                             g_task_get_cancellable (tree->task),
                             clear_tree_child_deleted, tree);
      }
      return;
    }

  {
    GFileInfo *info = items->data;
    g_autoptr (GFile) child = g_file_get_child (
      frame->directory, g_file_info_get_name (info));
    GFileType type = g_file_info_get_file_type (info);

    g_list_free_full (items, g_object_unref);
    if (type == G_FILE_TYPE_DIRECTORY)
      {
        ClearFrame *child_frame = g_new0 (ClearFrame, 1);
        child_frame->directory = g_steal_pointer (&child);
        g_ptr_array_add (tree->frames, child_frame);
        g_file_enumerate_children_async (
          child_frame->directory, G_FILE_ATTRIBUTE_STANDARD_NAME ","
          G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
          G_PRIORITY_DEFAULT, g_task_get_cancellable (tree->task),
          clear_tree_opened, tree);
      }
    else
      g_file_delete_async (child, G_PRIORITY_DEFAULT,
                           g_task_get_cancellable (tree->task),
                           clear_tree_deleted, tree);
  }
}

static void
clear_tree_next (ClearTree *tree)
{
  ClearFrame *frame = clear_tree_current (tree);

  g_file_enumerator_next_files_async (
    frame->enumerator, 1, G_PRIORITY_DEFAULT,
    g_task_get_cancellable (tree->task), clear_tree_file_ready, tree);
}

void
aml_cache_clear_all_async (GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, NULL);
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  ClearTree *tree = g_new0 (ClearTree, 1);
  ClearFrame *root = g_new0 (ClearFrame, 1);

  tree->task = g_steal_pointer (&task);
  tree->frames = g_ptr_array_new ();
  root->directory = g_file_new_for_path (path);
  g_ptr_array_add (tree->frames, root);
  g_file_enumerate_children_async (
    root->directory, G_FILE_ATTRIBUTE_STANDARD_NAME ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
    G_PRIORITY_DEFAULT, cancellable, clear_tree_opened, tree);
}
