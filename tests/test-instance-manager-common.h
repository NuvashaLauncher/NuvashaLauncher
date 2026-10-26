/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncherInstanceManager: persistence, atomic saves, duplication,
 * deletion and path validation. Only temporary directories are used
 * (never the real data directory) and everything runs headless: no
 * application is ever launched. */

#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-fs.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  GError *error;
  gpointer result;
  gboolean success;
  gboolean timed_out;
  guint watchdog;
  nuvashalauncherTaskScope *operation;
} AsyncCall;

static char *
make_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("nuvashalauncher-test-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);
  return dir;
}

/* Synchronous recursive delete, test cleanup only. Symlinks are
 * removed, not followed. */
static void
remove_recursive (const char *path)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInfo) info =
    g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);

  if (info == NULL)
    return;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr (GFileEnumerator) enumerator =
        g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   NULL, &error);

      if (enumerator != NULL)
        {
          GFileInfo *child = NULL;

          while ((child =
                    g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
            {
              g_autoptr (GFile) child_file =
                g_file_get_child (file, g_file_info_get_name (child));
              g_autofree char *child_path = g_file_get_path (child_file);
              g_object_unref (child);
              remove_recursive (child_path);
            }
        }
    }

  g_file_delete (file, NULL, NULL);
}

static void
write_file (const char *path, const char *contents)
{
  g_autofree char *parent = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);

  if (!g_file_set_contents (path, contents, -1, &error))
    g_error ("cannot write %s: %s", path, error->message);
}

static char *
read_file (const char *path)
{
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("cannot read %s: %s", path, error->message);

  return contents;
}

static guint
count_entries (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  guint count = 0;

  if (dir == NULL)
    return 0;

  while (g_dir_read_name (dir) != NULL)
    count++;

  g_dir_close (dir);
  return count;
}

static char *
prop_string (nuvashalauncherInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

static guint
manager_count (nuvashalauncherInstanceManager *manager)
{
  return g_list_model_get_n_items (
    G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (manager)));
}

static gboolean
async_timeout (gpointer user_data)
{
  AsyncCall *call = user_data;

  call->timed_out = TRUE;
  g_main_loop_quit (call->loop);
  return G_SOURCE_REMOVE;
}

/* Runs the main loop until the pending operation reports back, with a
 * watchdog so a lost callback fails the test instead of hanging. */
static void
async_call_run (AsyncCall *call)
{
  call->watchdog = g_timeout_add_seconds (15, async_timeout, call);
  g_main_loop_run (call->loop);

  if (!call->timed_out)
    g_source_remove (call->watchdog);

  g_assert_false (call->timed_out);
}

static void
async_call_clear (AsyncCall *call)
{
  /* GMainLoop is refcounted but is not a GObject, so g_clear_object
   * would not work here. */
  if (call->loop != NULL)
    {
      g_main_loop_unref (call->loop);
      call->loop = NULL;
    }

  g_clear_error (&call->error);
}

static void
duplicate_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;

  call->result =
    nuvashalauncher_instance_manager_duplicate_finish (nuvashalauncher_INSTANCE_MANAGER (source),
                                           result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
delete_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;

  call->success =
    nuvashalauncher_instance_manager_delete_finish (nuvashalauncher_INSTANCE_MANAGER (source),
                                        result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
open_folder_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;

  call->success =
    nuvashalauncher_instance_manager_open_folder_finish (nuvashalauncher_INSTANCE_MANAGER (source),
                                             result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
change_directory_finished (GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
  AsyncCall *call = user_data;

  call->success = nuvashalauncher_instance_manager_change_directory_finish (
    nuvashalauncher_INSTANCE_MANAGER (source), result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

/* ---------------------------------------------------------- *
 * Shared test helpers end here.
 */
