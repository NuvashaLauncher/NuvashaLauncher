/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-index.h"

#include <glib.h>
#include <glib/gstdio.h>

/* One per update, because each operation owns its own handle: which of
   the two answers arrives first is not decided here. */
typedef struct {
  GMainLoop *loop;
  guint *completed;
  nuvashalauncherTaskScope *operation;
} UpdateWait;

static void
update_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  UpdateWait *wait = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_content_index_modify_finish (result, &error))
    g_error ("content index update failed: %s", error->message);

  (*wait->completed)++;
  if (*wait->completed == 2)
    g_main_loop_quit (wait->loop);

  /* The handle released only once the answer is in. */
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_free (wait);
}

typedef struct {
  GMainLoop *loop;
  nuvashalauncherTaskScope *operation;
} LoadWait;

typedef struct {
  GMainLoop *loop;
  nuvashalauncherTaskScope *operation;
  GError *error;
} ErrorWait;

static void
load_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
           gpointer user_data)
{
  LoadWait *wait = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) entries =
    nuvashalauncher_content_index_load_finish (result, &error);

  g_assert_no_error (error);
  g_assert_nonnull (entries);
  g_assert_cmpuint (entries->len, ==, 2);

  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
  g_free (wait);
}

static void
load_error_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  ErrorWait *wait = user_data;
  g_autoptr (GPtrArray) entries =
    nuvashalauncher_content_index_load_finish (result, &wait->error);

  g_assert_null (entries);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

static void
start_update (const char *root, nuvashalauncherContentIndexEntry *entry, GMainLoop *loop,
              guint *completed)
{
  UpdateWait *wait = g_new0 (UpdateWait, 1);

  wait->loop = loop;
  wait->completed = completed;
  wait->operation = nuvashalauncher_content_index_update_async (root, nuvashalauncher_CONTENT_MOD,
                                                     entry, update_done, wait);
}

static void
test_concurrent_updates_are_serialized (void)
{
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-content-index-XXXXXX", NULL);
  g_autofree char *index_directory = g_build_filename (root, "mods", ".index",
                                                       NULL);
  g_autofree char *first_path = NULL;
  g_autofree char *second_path = NULL;
  g_autofree char *mods_directory = g_build_filename (root, "mods", NULL);
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  guint completed = 0;
  nuvashalauncherContentIndexEntry first = {
    .filename = "first.jar",
    .hash = "first-sha1",
    .hash512 = "first-sha512",
    .download_url = "https://cdn.example/first.jar",
    .env_client = "required",
    .env_server = "required",
    .project_id = "first",
    .project_name = "First",
    .version_id = "first-version",
    .version_name = "1.0",
  };
  nuvashalauncherContentIndexEntry second = {
    .filename = "second.jar",
    .hash = "second-sha1",
    .hash512 = "second-sha512",
    .download_url = "https://cdn.example/second.jar",
    .env_client = "required",
    .env_server = "required",
    .project_id = "second",
    .project_name = "Second",
    .version_id = "second-version",
    .version_name = "1.0",
  };

  g_assert_cmpint (g_mkdir (mods_directory, 0755), ==, 0);

  start_update (root, &first, loop, &completed);
  start_update (root, &second, loop, &completed);
  g_main_loop_run (loop);

  first_path = nuvashalauncher_content_index_entry_path (root, nuvashalauncher_CONTENT_MOD,
                                             first.filename);
  second_path = nuvashalauncher_content_index_entry_path (root, nuvashalauncher_CONTENT_MOD,
                                              second.filename);
  g_assert_true (g_file_test (first_path, G_FILE_TEST_IS_REGULAR));
  g_assert_true (g_file_test (second_path, G_FILE_TEST_IS_REGULAR));

  {
    g_autofree char *metadata = NULL;
    gsize length = 0;

    g_assert_true (g_file_get_contents (first_path, &metadata, &length, NULL));
    g_assert_nonnull (g_strstr_len (metadata, (gssize) length,
                                    "\"version\" : 2"));
  }

  {
    LoadWait *load = g_new0 (LoadWait, 1);

    load->loop = loop;
    load->operation = nuvashalauncher_content_index_load_async (root, nuvashalauncher_CONTENT_MOD,
                                                    load_done, load);
  }
  g_main_loop_run (loop);

  g_main_loop_unref (loop);
  g_remove (first_path);
  g_remove (second_path);
  g_rmdir (index_directory);
  {
    g_rmdir (mods_directory);
  }
  g_rmdir (root);
}

static void
test_invalid_entry_fails_without_partial_results (void)
{
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-content-index-bad-XXXXXX", NULL);
  g_autofree char *directory = g_build_filename (root, "mods", ".index", NULL);
  g_autofree char *mods = g_path_get_dirname (directory);
  g_autofree char *path = g_build_filename (directory, "broken.json", NULL);
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  ErrorWait wait = { .loop = loop };

  g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path,
                                      "{\"version\":1,\"filename\":\"x.jar\"}",
                                      -1, NULL));
  wait.operation = nuvashalauncher_content_index_load_async (
    root, nuvashalauncher_CONTENT_MOD, load_error_done, &wait);
  g_main_loop_run (loop);

  g_assert_null (wait.operation);
  g_assert_error (wait.error, nuvashalauncher_CONTENT_INDEX_ERROR,
                  nuvashalauncher_CONTENT_INDEX_ERROR_INVALID);
  g_clear_error (&wait.error);
  g_main_loop_unref (loop);
  g_remove (path);
  g_rmdir (directory);
  g_rmdir (mods);
  g_rmdir (root);
  g_print ("ok - an invalid content index fails atomically\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/content-index/concurrent-updates",
                   test_concurrent_updates_are_serialized);
  g_test_add_func ("/content-index/invalid-entry",
                   test_invalid_entry_fails_without_partial_results);
  return g_test_run ();
}
