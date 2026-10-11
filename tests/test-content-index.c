/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-index.h"

#include <glib.h>
#include <glib/gstdio.h>

typedef struct {
  GMainLoop *loop;
  guint completed;
} UpdateWait;

static void
update_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  UpdateWait *wait = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_content_index_modify_finish (result, &error))
    g_error ("content index update failed: %s", error->message);

  wait->completed++;
  if (wait->completed == 2)
    g_main_loop_quit (wait->loop);
}

static void
load_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
           gpointer user_data)
{
  GMainLoop *loop = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) entries =
    aml_content_index_load_finish (result, &error);

  g_assert_no_error (error);
  g_assert_nonnull (entries);
  g_assert_cmpuint (entries->len, ==, 2);
  g_main_loop_quit (loop);
}

static void
test_concurrent_updates_are_serialized (void)
{
  g_autofree char *root = g_dir_make_tmp ("aml-content-index-XXXXXX", NULL);
  g_autofree char *index_directory = g_build_filename (root, "mods", ".index",
                                                       NULL);
  g_autofree char *first_path = NULL;
  g_autofree char *second_path = NULL;
  g_autofree char *mods_directory = g_build_filename (root, "mods", NULL);
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  UpdateWait wait = { loop, 0 };
  AmlContentIndexEntry first = {
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
  AmlContentIndexEntry second = {
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

  aml_content_index_update_async (root, AML_CONTENT_MOD, &first, NULL,
                                  update_done, &wait);
  aml_content_index_update_async (root, AML_CONTENT_MOD, &second, NULL,
                                  update_done, &wait);
  g_main_loop_run (loop);

  first_path = aml_content_index_entry_path (root, AML_CONTENT_MOD,
                                             first.filename);
  second_path = aml_content_index_entry_path (root, AML_CONTENT_MOD,
                                              second.filename);
  g_assert_true (g_file_test (first_path, G_FILE_TEST_IS_REGULAR));
  g_assert_true (g_file_test (second_path, G_FILE_TEST_IS_REGULAR));

  aml_content_index_load_async (root, AML_CONTENT_MOD, NULL, load_done, loop);
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/content-index/concurrent-updates",
                   test_concurrent_updates_are_serialized);
  return g_test_run ();
}
