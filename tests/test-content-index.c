/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-index.h"

#include <glib.h>
#include <glib/gstdio.h>

typedef struct {
  GMainLoop *loop;
  GPtrArray *entries;
  GError *error;
  gboolean success;
} Wait;

static void
load_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
  Wait *wait = data;
  wait->entries = aml_content_index_load_finish (result, &wait->error);
  g_main_loop_quit (wait->loop);
}

static void
write_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
  Wait *wait = data;
  wait->success = g_task_propagate_boolean (G_TASK (result), &wait->error);
  g_main_loop_quit (wait->loop);
}

static void
wait_clear (Wait *wait)
{
  g_clear_pointer (&wait->entries, g_ptr_array_unref);
  g_clear_error (&wait->error);
  g_clear_pointer (&wait->loop, g_main_loop_unref);
}

static void
load (const char *root, Wait *wait)
{
  wait->loop = g_main_loop_new (NULL, FALSE);
  aml_content_index_load_async (root, AML_CONTENT_MOD, NULL, load_done, wait);
  g_main_loop_run (wait->loop);
}

static void
modify (const char *root, AmlContentIndexEntry *entry, const char *remove,
        Wait *wait)
{
  wait->loop = g_main_loop_new (NULL, FALSE);
  if (entry != NULL)
    aml_content_index_update_async (root, AML_CONTENT_MOD, entry, NULL,
                                    write_done, wait);
  else
    aml_content_index_remove_async (root, AML_CONTENT_MOD, remove, NULL,
                                    write_done, wait);
  g_main_loop_run (wait->loop);
}

static AmlContentIndexEntry *
entry_new (const char *filename, const char *hash, const char *project,
           const char *version)
{
  AmlContentIndexEntry *entry = g_new0 (AmlContentIndexEntry, 1);
  entry->filename = g_strdup (filename);
  entry->hash = g_strdup (hash);
  entry->provider = g_strdup ("Modrinth");
  entry->project_id = g_strdup (project);
  entry->project_name = g_strdup ("Example Project");
  entry->version_id = g_strdup (version);
  entry->version_name = g_strdup ("1.0");
  entry->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  entry->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  entry->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (entry->required_dependencies, g_strdup ("required"));
  g_ptr_array_add (entry->optional_dependencies, g_strdup ("optional"));
  g_ptr_array_add (entry->incompatible_dependencies, g_strdup ("incompatible"));
  return entry;
}

static void
test_index (void)
{
  g_autofree char *root = g_dir_make_tmp ("aml-index-XXXXXX", NULL);
  g_autofree char *folder = g_build_filename (root, "mods", NULL);
  g_autoptr (AmlContentIndexEntry) entry =
    entry_new ("example.jar", "hash-one", "project", "version-one");
  g_autoptr (AmlContentIndexEntry) duplicate =
    entry_new ("example-two.jar", "hash-two", "project", "version-two");
  Wait wait = { 0 };

  g_mkdir_with_parents (folder, 0755);
  load (root, &wait);
  g_assert_no_error (wait.error);
  g_assert_cmpuint (wait.entries->len, ==, 0);
  wait_clear (&wait);

  modify (root, entry, NULL, &wait);
  g_assert_true (wait.success);
  wait_clear (&wait);
  load (root, &wait);
  g_assert_no_error (wait.error);
  g_assert_cmpuint (wait.entries->len, ==, 1);
  AmlContentIndexEntry *read = g_ptr_array_index (wait.entries, 0);
  g_assert_cmpstr (read->hash, ==, "hash-one");
  g_assert_cmpuint (read->required_dependencies->len, ==, 1);
  wait_clear (&wait);

  modify (root, duplicate, NULL, &wait);
  g_assert_true (wait.success);
  wait_clear (&wait);
  load (root, &wait);
  g_assert_cmpuint (wait.entries->len, ==, 2);
  g_assert_cmpstr (((AmlContentIndexEntry *) g_ptr_array_index (
                     wait.entries, 1))->project_id, ==, "project");
  wait_clear (&wait);
  modify (root, NULL, "example-two.jar", &wait);
  g_assert_true (wait.success);
  wait_clear (&wait);

  {
    g_autofree char *index_path =
      g_build_filename (folder, ".index", "index.json", NULL);
    g_assert_true (g_file_set_contents (index_path, "broken", -1, NULL));
  }
  load (root, &wait);
  g_assert_error (wait.error, AML_CONTENT_INDEX_ERROR,
                  AML_CONTENT_INDEX_ERROR_INVALID);
  wait_clear (&wait);

  modify (root, entry, NULL, &wait);
  g_assert_true (wait.success);
  wait_clear (&wait);
  g_free (entry->hash);
  entry->hash = g_strdup ("hash-two");
  modify (root, entry, NULL, &wait);
  g_assert_true (wait.success);
  wait_clear (&wait);
  load (root, &wait);
  g_assert_cmpstr (((AmlContentIndexEntry *) g_ptr_array_index (
                     wait.entries, 0))->hash, ==, "hash-two");
  wait_clear (&wait);

  modify (root, NULL, "example.jar", &wait);
  g_assert_true (wait.success);
  wait_clear (&wait);
  load (root, &wait);
  g_assert_cmpuint (wait.entries->len, ==, 0);
  wait_clear (&wait);

  g_print ("ok - content index persists, repairs, validates and deletes\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/content-index/index", test_index);
  return g_test_run ();
}
