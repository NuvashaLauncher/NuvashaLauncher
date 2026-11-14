/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-task-private.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#define CACHE_SUBDIR "nuvashalauncher"
#define CACHE_API_MAX_BYTES (64u * 1024u * 1024u)
#define CACHE_ICONS_MAX_BYTES (64u * 1024u * 1024u)
#define CACHE_FILES_MAX_BYTES (512u * 1024u * 1024u)
#define CACHE_API_MIN_STALE_SECONDS (7 * 24 * 60 * 60)
#define CACHE_ICONS_MAX_AGE_SECONDS (30 * 24 * 60 * 60)

static char *
cache_directory (const char *child)
{
  return g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, child, NULL);
}

char *
nuvashalauncher_cache_get_api_directory (void)
{
  return cache_directory ("api");
}

char *
nuvashalauncher_cache_get_files_directory (void)
{
  return cache_directory ("files");
}

static nuvashalauncherTaskScope *
cache_clear_async (const char *path, GAsyncReadyCallback callback,
                   gpointer user_data)
{
  return nuvashalauncher_fs_remove_tree_async (path, callback, user_data);
}

nuvashalauncherTaskScope *
nuvashalauncher_cache_clear_all_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, NULL);
  return cache_clear_async (path, callback, user_data);
}

typedef struct
{
  char *path;
  guint64 size;
  gint64 mtime;
} PruneEntry;

static void
prune_entry_free (gpointer data)
{
  PruneEntry *entry = data;

  if (entry == NULL)
    return;
  g_free (entry->path);
  g_free (entry);
}

static int
prune_entry_compare_oldest (gconstpointer a, gconstpointer b)
{
  const PruneEntry *ea = *(const PruneEntry * const *) a;
  const PruneEntry *eb = *(const PruneEntry * const *) b;

  if (ea->mtime < eb->mtime)
    return -1;
  if (ea->mtime > eb->mtime)
    return 1;
  return 0;
}

static void
prune_directory (const char   *directory,
                 gint64        max_age_seconds,
                 guint64       max_bytes,
                 GCancellable *cancellable)
{
  GDir *dir = g_dir_open (directory, 0, NULL);
  const char *name = NULL;
  g_autoptr (GPtrArray) entries =
    g_ptr_array_new_with_free_func (prune_entry_free);
  guint64 total_bytes = 0;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  if (dir == NULL)
    return;

  while (!g_cancellable_is_cancelled (cancellable) &&
         (name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *path = g_build_filename (directory, name, NULL);
      GStatBuf st;

      if (g_lstat (path, &st) != 0 || !S_ISREG (st.st_mode))
        continue;

      if (max_age_seconds > 0 && now - (gint64) st.st_mtime > max_age_seconds)
        {
          g_unlink (path);
          continue;
        }

      PruneEntry *entry = g_new0 (PruneEntry, 1);
      entry->path = g_steal_pointer (&path);
      entry->size = st.st_size > 0 ? (guint64) st.st_size : 0;
      entry->mtime = (gint64) st.st_mtime;
      total_bytes += entry->size;
      g_ptr_array_add (entries, entry);
    }
  g_dir_close (dir);

  if (max_bytes > 0 && total_bytes > max_bytes &&
      !g_cancellable_is_cancelled (cancellable))
    {
      g_ptr_array_sort (entries, prune_entry_compare_oldest);
      for (guint i = 0; i < entries->len && total_bytes > max_bytes; i++)
        {
          PruneEntry *entry = g_ptr_array_index (entries, i);
          if (g_unlink (entry->path) == 0)
            total_bytes -= MIN (total_bytes, entry->size);
        }
    }
}

static void
cache_prune_thread (GTask        *task,
                    gpointer      source_object G_GNUC_UNUSED,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
  gint64 api_max_age = *(gint64 *) task_data;
  g_autofree char *api_dir = nuvashalauncher_cache_get_api_directory ();
  g_autofree char *icons_dir =
    g_build_filename (g_get_user_cache_dir (), CACHE_SUBDIR, "icons",
                      "external", NULL);
  g_autofree char *files_dir = nuvashalauncher_cache_get_files_directory ();

  prune_directory (api_dir, MAX (api_max_age, CACHE_API_MIN_STALE_SECONDS),
                   CACHE_API_MAX_BYTES, cancellable);
  prune_directory (icons_dir, CACHE_ICONS_MAX_AGE_SECONDS,
                   CACHE_ICONS_MAX_BYTES, cancellable);
  prune_directory (files_dir, 0, CACHE_FILES_MAX_BYTES, cancellable);

  g_task_return_boolean (task, TRUE);
}

nuvashalauncherTaskScope *
nuvashalauncher_cache_prune_async (gint64              api_max_age_seconds,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvashalauncher_task_scope_task (scope);
  gint64 *data = g_new (gint64, 1);

  *data = api_max_age_seconds;
  g_task_set_task_data (task, data, g_free);
  g_task_run_in_thread (task, cache_prune_thread);
  return g_steal_pointer (&scope);
}
