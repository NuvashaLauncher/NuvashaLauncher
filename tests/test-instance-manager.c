/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for NuvashaInstanceManager: persistence, atomic saves, duplication,
 * deletion and path validation. Only temporary directories are used
 * (never the real data directory) and everything runs headless: no
 * application is ever launched. */

#include "nuvasha-instance-manager.h"
#include "nuvasha-fs.h"

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
  NuvashaTaskScope *operation;
} AsyncCall;

static char *
make_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("nuvasha-test-XXXXXX", &error);

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
prop_string (NuvashaInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

static guint
manager_count (NuvashaInstanceManager *manager)
{
  return g_list_model_get_n_items (
    G_LIST_MODEL (nuvasha_instance_manager_get_instances (manager)));
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
    nuvasha_instance_manager_duplicate_finish (nuvasha_INSTANCE_MANAGER (source),
                                           result, &call->error);
  nuvasha_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
delete_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;

  call->success =
    nuvasha_instance_manager_delete_finish (nuvasha_INSTANCE_MANAGER (source),
                                        result, &call->error);
  nuvasha_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
open_folder_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;

  call->success =
    nuvasha_instance_manager_open_folder_finish (nuvasha_INSTANCE_MANAGER (source),
                                             result, &call->error);
  nuvasha_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
change_directory_finished (GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
  AsyncCall *call = user_data;

  call->success = nuvasha_instance_manager_change_directory_finish (
    nuvasha_INSTANCE_MANAGER (source), result, &call->error);
  nuvasha_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

/* ---------------------------------------------------------- *
 * Tests
 * ---------------------------------------------------------- */

static void
test_create_and_persist (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);
  g_assert_nonnull (nuvasha_instance_manager_get_instances (manager));
  g_assert_cmpuint (manager_count (manager), ==, 0);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Pack One", NULL, "1.21.1",
                                 nuvasha_LOADER_FABRIC, "0.16.9", &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_assert_cmpuint (manager_count (manager), ==, 1);

   /* The default directory lives below <data>/instances/ and matches the
   * sanitized instance name. */
   g_autofree char *directory = prop_string (instance, "directory");
   g_autofree char *directory_name = g_path_get_basename (directory);
   g_autofree char *instances_root =
     g_build_filename (data_dir, "instances", NULL);
   g_assert_true (g_str_has_prefix (directory, instances_root));
   g_assert_cmpstr (directory_name, ==, "Pack One");
  g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

  /* The metadata file carries format version 2 and one entry. */
  g_autofree char *metadata_path =
    g_build_filename (data_dir, "instances.json", NULL);
  g_assert_true (g_file_test (metadata_path, G_FILE_TEST_EXISTS));

  g_autofree char *text = read_file (metadata_path);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, text, -1, &error));
  g_assert_no_error (error);

  JsonNode *root = json_parser_get_root (parser);
  g_assert_true (JSON_NODE_HOLDS_OBJECT (root));

  JsonObject *object = json_node_get_object (root);
  JsonNode *version = json_object_get_member (object, "version");
  g_assert_nonnull (version);
  g_assert_cmpint (json_node_get_value_type (version), ==, G_TYPE_INT64);
  g_assert_cmpint (json_node_get_int (version), ==, 2);

  JsonNode *array = json_object_get_member (object, "instances");
  g_assert_true (JSON_NODE_HOLDS_ARRAY (array));
  g_assert_cmpuint (json_array_get_length (json_node_get_array (array)), ==, 1);

  /* A fresh manager reads the same instance back. */
  g_autoptr (NuvashaInstanceManager) fresh = nuvasha_instance_manager_new (data_dir);
  g_assert_true (nuvasha_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 1);

  g_autoptr (NuvashaInstance) loaded =
    g_list_model_get_item (
      G_LIST_MODEL (nuvasha_instance_manager_get_instances (fresh)), 0);
  g_assert_cmpstr (nuvasha_instance_get_id (loaded), ==,
                   nuvasha_instance_get_id (instance));

  g_autofree char *loaded_name = prop_string (loaded, "name");
  g_autofree char *loaded_version = prop_string (loaded, "minecraft-version");
  g_autofree char *loaded_loader_version =
    prop_string (loaded, "loader-version");
  g_autofree char *loaded_directory = prop_string (loaded, "directory");

  g_assert_cmpstr (loaded_name, ==, "Pack One");
  g_assert_cmpstr (loaded_version, ==, "1.21.1");
  g_assert_cmpstr (loaded_loader_version, ==, "0.16.9");
  g_assert_cmpstr (loaded_directory, ==, directory);

  g_assert_true (nuvasha_instance_manager_get_by_id (
    fresh, nuvasha_instance_get_id (instance)) == loaded);

  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - create and persistence\n");
}

static void
test_create_explicit_directory (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autofree char *chosen = g_build_filename (data_dir, "chosen", NULL);
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Chosen", chosen, "1.20.4",
                                 nuvasha_LOADER_QUILT, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_assert_cmpstr (directory, ==, chosen);
  g_assert_true (g_file_test (chosen, G_FILE_TEST_IS_DIR));

  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - explicit directory\n");
}

static void
test_create_sanitized_name (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  /* A name that cannot be used as a directory component is rejected rather
   * than silently changed. */
  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "../../evil", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_assert_cmpstr (error->message, ==,
                   "Instances cannot contain / in their name");
  g_clear_error (&error);
  remove_recursive (data_dir);

  g_print ("ok - unusable instance names are refused\n");
}

static void
test_create_duplicate_names (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *first =
    nuvasha_instance_manager_create (manager, "test", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);

  NuvashaInstance *second =
    nuvasha_instance_manager_create (manager, "test", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (second);

  NuvashaInstance *third =
    nuvasha_instance_manager_create (manager, "test", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (third);

  g_autofree char *first_name = prop_string (first, "name");
  g_autofree char *second_name = prop_string (second, "name");
  g_autofree char *third_name = prop_string (third, "name");
  g_autofree char *first_directory = prop_string (first, "directory");
  g_autofree char *second_directory = prop_string (second, "directory");
  g_autofree char *third_directory = prop_string (third, "directory");
  g_autofree char *first_leaf = g_path_get_basename (first_directory);
  g_autofree char *second_leaf = g_path_get_basename (second_directory);
  g_autofree char *third_leaf = g_path_get_basename (third_directory);

  g_assert_cmpstr (first_name, ==, "test");
   g_assert_cmpstr (second_name, ==, "test 1");
   g_assert_cmpstr (third_name, ==, "test 2");
  g_assert_cmpstr (first_leaf, ==, first_name);
  g_assert_cmpstr (second_leaf, ==, second_name);
  g_assert_cmpstr (third_leaf, ==, third_name);

  g_object_unref (first);
  g_object_unref (second);
  g_object_unref (third);
  remove_recursive (data_dir);

  g_print ("ok - duplicate names get numeric suffixes\n");
}

static void
test_path_validation (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *first =
    nuvasha_instance_manager_create (manager, "One", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);

  g_autofree char *first_directory = prop_string (first, "directory");

  /* A relative path is refused. */
  g_autoptr (NuvashaInstance) relative =
    nuvasha_instance_manager_create (manager, "Two", "relative/path", "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_null (relative);
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* A path with ".." inside is refused and never created. */
  g_autofree char *escape =
    g_build_filename (data_dir, "instances", "..", "escape", NULL);
  g_autoptr (NuvashaInstance) escaping =
    nuvasha_instance_manager_create (manager, "Three", escape, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_null (escaping);
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  g_autofree char *escape_canonical = g_canonicalize_filename (escape, NULL);
  g_assert_false (g_file_test (escape_canonical, G_FILE_TEST_EXISTS));

  /* The filesystem root is refused. */
  g_autoptr (NuvashaInstance) root =
    nuvasha_instance_manager_create (manager, "Four", G_DIR_SEPARATOR_S, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_null (root);
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* The directory of an existing instance is refused. */
  g_autoptr (NuvashaInstance) same =
    nuvasha_instance_manager_create (manager, "Five", first_directory, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_null (same);
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* A directory containing an existing instance is refused. */
  g_autofree char *parent = g_path_get_dirname (first_directory);
  g_autoptr (NuvashaInstance) containing =
    nuvasha_instance_manager_create (manager, "Six", parent, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_null (containing);
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* A directory inside an existing instance is refused too. */
  g_autofree char *inside =
    g_build_filename (first_directory, "nested", NULL);
  g_autoptr (NuvashaInstance) nested =
    nuvasha_instance_manager_create (manager, "Seven", inside, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_null (nested);
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);
  g_assert_false (g_file_test (inside, G_FILE_TEST_EXISTS));

  /* None of the refused paths changed the list. */
  g_assert_cmpuint (manager_count (manager), ==, 1);

  g_object_unref (first);
  remove_recursive (data_dir);

  g_print ("ok - path validation\n");
}

static void
test_load_missing_file (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  /* First run: no file at all is a success with an empty list. */
  g_assert_true (nuvasha_instance_manager_load (manager, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (manager), ==, 0);

  remove_recursive (data_dir);

  g_print ("ok - missing metadata file\n");
}

static void
test_load_removes_missing_directory (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Removed", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_autofree char *directory = prop_string (instance, "directory");
  g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

  remove_recursive (directory);

  g_autoptr (NuvashaInstanceManager) fresh =
    nuvasha_instance_manager_new (data_dir);
  g_assert_true (nuvasha_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 0);

  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - missing instance directories are removed\n");
}

static void
assert_quarantined (const char *data_dir, const char *original_text)
{
  g_autofree char *metadata_path =
    g_build_filename (data_dir, "instances.json", NULL);
  g_autofree char *corrupt_path =
    g_build_filename (data_dir, "instances.json.corrupt", NULL);

  /* The unreadable file is kept, never deleted, and the real file is
   * gone so the next start starts empty. */
  g_assert_false (g_file_test (metadata_path, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (corrupt_path, G_FILE_TEST_EXISTS));

  g_autofree char *kept = read_file (corrupt_path);
  g_assert_cmpstr (kept, ==, original_text);
}

static void
test_corrupt_file (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autofree char *metadata_path =
    g_build_filename (data_dir, "instances.json", NULL);
  const char *garbage = "this is { not json";
  write_file (metadata_path, garbage);

  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  g_assert_false (nuvasha_instance_manager_load (manager, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  assert_quarantined (data_dir, garbage);

  remove_recursive (data_dir);

  g_print ("ok - corrupt metadata is quarantined\n");
}

static void
test_missing_field_file (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autofree char *metadata_path =
    g_build_filename (data_dir, "instances.json", NULL);
  g_autofree char *text =
    g_strdup ("{\"version\": 2, \"instances\": [{"
              "\"id\": \"11111111-2222-3333-4444-555555555555\","
              "\"name\": \"Broken\"}]}");
  write_file (metadata_path, text);

  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  /* An entry without the required fields is a corrupt file, reported
   * as an error instead of crashing. */
  g_assert_false (nuvasha_instance_manager_load (manager, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  assert_quarantined (data_dir, text);

  remove_recursive (data_dir);

  g_print ("ok - instance with missing fields\n");
}

static void
test_version_mismatch (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autofree char *metadata_path =
    g_build_filename (data_dir, "instances.json", NULL);
  const char *text = "{\"version\": 1, \"instances\": []}";
  write_file (metadata_path, text);

  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  g_assert_false (nuvasha_instance_manager_load (manager, &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT);
  g_clear_error (&error);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  assert_quarantined (data_dir, text);

  remove_recursive (data_dir);

  g_print ("ok - unsupported metadata version\n");
}

static void
test_atomic_save_failure (void)
{
  if (geteuid () == 0)
    {
      g_print ("skip - a read only directory does not stop root\n");
      return;
    }

  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Durable", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *metadata_path =
    g_build_filename (data_dir, "instances.json", NULL);
  g_autofree char *before = read_file (metadata_path);
  g_assert_cmpuint (count_entries (data_dir), ==, 2);

  /* Without write permission on the directory the save must fail
   * without touching the old file and without leaving a temporary
   * file behind. */
  g_assert_cmpint (g_chmod (data_dir, 0500), ==, 0);

  g_assert_false (nuvasha_instance_manager_save (manager, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_assert_cmpint (g_chmod (data_dir, 0700), ==, 0);

  g_autofree char *after = read_file (metadata_path);
  g_assert_cmpstr (after, ==, before);
  g_assert_cmpuint (count_entries (data_dir), ==, 2);

  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - failed save leaves the old file intact\n");
}

static void
test_rename (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Original", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_assert_true (nuvasha_instance_manager_rename (manager, instance,
                                              "  Renamed Pack  ", &error));
  g_assert_no_error (error);

  g_autofree char *name = prop_string (instance, "name");
  g_assert_cmpstr (name, ==, "Renamed Pack");

  /* The new name is on disk. */
  g_autoptr (NuvashaInstanceManager) fresh = nuvasha_instance_manager_new (data_dir);
  g_assert_true (nuvasha_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 1);

  g_autoptr (NuvashaInstance) loaded =
    g_list_model_get_item (
      G_LIST_MODEL (nuvasha_instance_manager_get_instances (fresh)), 0);
  g_autofree char *loaded_name = prop_string (loaded, "name");
  g_assert_cmpstr (loaded_name, ==, "Renamed Pack");

  /* A blank name is refused and the old name survives. */
  g_assert_false (nuvasha_instance_manager_rename (manager, instance, "   ",
                                               &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_free (name);
  name = prop_string (instance, "name");
  g_assert_cmpstr (name, ==, "Renamed Pack");

  /* A path separator is reported specifically and leaves the old name. */
  g_assert_false (nuvasha_instance_manager_rename (manager, instance, "/",
                                               &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_assert_cmpstr (error->message, ==,
                   "Instances cannot contain / in their name");
  g_clear_error (&error);

  /* Renaming an instance the manager does not know fails. */
  g_autoptr (NuvashaInstance) stranger =
    nuvasha_instance_new (NULL, "Stranger", "/tmp/nuvasha-test-stranger", "1.21.1",
                      nuvasha_LOADER_VANILLA, &error);
  g_assert_no_error (error);
  g_assert_nonnull (stranger);

  g_assert_false (nuvasha_instance_manager_rename (manager, stranger,
                                               "Renamed", &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_NOT_FOUND);
  g_clear_error (&error);

  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - rename\n");
}

static void
test_model_updates_are_atomic (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);
  g_autoptr (NuvashaInstance) instance =
    nuvasha_instance_manager_create (manager, "Model", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);

  g_assert_no_error (error);
  g_assert_true (nuvasha_instance_manager_set_string (
    manager, instance, "jvm-args", "-Xmx2G", &error));
  g_assert_no_error (error);
  g_assert_true (nuvasha_instance_manager_set_loader (
    manager, instance, nuvasha_LOADER_FABRIC, "0.15.0", &error));
  g_assert_no_error (error);
  g_assert_false (nuvasha_instance_manager_set_string (
    manager, instance, "not-a-property", "value", &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  g_autoptr (NuvashaInstanceManager) fresh = nuvasha_instance_manager_new (data_dir);
  g_assert_true (nuvasha_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_autoptr (NuvashaInstance) loaded = g_list_model_get_item (
    G_LIST_MODEL (nuvasha_instance_manager_get_instances (fresh)), 0);
  g_autofree char *args = prop_string (loaded, "jvm-args");
  g_assert_cmpstr (args, ==, "-Xmx2G");

  remove_recursive (data_dir);
  g_print ("ok - model updates validate and persist atomically\n");
}

static void
test_duplicate (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Original", NULL, "1.21.1",
                                 nuvasha_LOADER_FABRIC, "0.16.9", &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_autofree char *options_path =
    g_build_filename (directory, "options.txt", NULL);
  write_file (options_path, "fov:90");

  g_autofree char *level_path =
    g_build_filename (directory, "saves", "world", "level.dat", NULL);
  write_file (level_path, "level data");

  g_autofree char *link_path = g_build_filename (directory, "link", NULL);
  g_autoptr (GFile) link = g_file_new_for_path (link_path);
  g_assert_true (g_file_make_symbolic_link (link, "options.txt", NULL,
                                            &error));
  g_assert_no_error (error);

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvasha_instance_manager_duplicate_async (
     manager, instance, duplicate_finished, &call);
  async_call_run (&call);
  g_assert_no_error (call.error);
  g_assert_nonnull (call.result);

  NuvashaInstance *copy = call.result;
  g_assert_cmpstr (nuvasha_instance_get_id (copy), !=,
                   nuvasha_instance_get_id (instance));
  g_assert_cmpstr (nuvasha_instance_get_id (copy), !=, "");
  g_assert_true (g_uuid_string_is_valid (nuvasha_instance_get_id (copy)));

  g_autofree char *copy_name = prop_string (copy, "name");
  g_autofree char *copy_directory = prop_string (copy, "directory");
  g_autofree char *copy_loader_version = prop_string (copy, "loader-version");
  g_assert_cmpstr (copy_name, ==, "Copy of Original");
  g_assert_cmpstr (copy_directory, !=, directory);
  g_assert_cmpstr (copy_loader_version, ==, "0.16.9");

  /* The files were copied, including the nested directory. */
  g_autofree char *copied_options_path =
    g_build_filename (copy_directory, "options.txt", NULL);
  g_autofree char *copied_options = read_file (copied_options_path);
  g_assert_cmpstr (copied_options, ==, "fov:90");

  g_autofree char *copied_level_path =
    g_build_filename (copy_directory, "saves", "world", "level.dat", NULL);
  g_autofree char *copied_level = read_file (copied_level_path);
  g_assert_cmpstr (copied_level, ==, "level data");

  /* The symlink still is a symlink pointing at its own copy. */
  g_autofree char *copied_link_path =
    g_build_filename (copy_directory, "link", NULL);
  g_assert_true (g_file_test (copied_link_path, G_FILE_TEST_IS_SYMLINK));

  g_autofree char *copied_link_contents = read_file (copied_link_path);
  g_assert_cmpstr (copied_link_contents, ==, "fov:90");

  /* Both instances are registered and both survive a reload. */
  g_assert_cmpuint (manager_count (manager), ==, 2);

  g_autoptr (NuvashaInstanceManager) fresh = nuvasha_instance_manager_new (data_dir);
  g_assert_true (nuvasha_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 2);
  g_assert_nonnull (nuvasha_instance_manager_get_by_id (
    fresh, nuvasha_instance_get_id (instance)));
  g_assert_nonnull (nuvasha_instance_manager_get_by_id (
    fresh, nuvasha_instance_get_id (copy)));

  async_call_clear (&call);
  g_object_unref (copy);
  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - duplication\n");
}

static void
test_duplicate_without_directory (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Vanishing", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_autofree char *instances_root =
    g_build_filename (data_dir, "instances", NULL);
  g_autofree char *icon = g_build_filename (directory, "icon.png", NULL);

  g_assert_cmpuint (count_entries (instances_root), ==, 1);
  /* New instances receive the default icon, so remove that generated file
   * before simulating an externally deleted instance directory. */
  g_assert_true (nuvasha_fs_unlink (icon, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
  g_assert_cmpuint (count_entries (instances_root), ==, 0);

  /* Copying a directory that is gone fails with an error, creates
   * nothing and keeps the list untouched. */
  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvasha_instance_manager_duplicate_async (
     manager, instance, duplicate_finished, &call);
  async_call_run (&call);
  g_assert_error (call.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_null (call.result);
  g_assert_cmpuint (manager_count (manager), ==, 1);
  g_assert_cmpuint (count_entries (instances_root), ==, 0);

  async_call_clear (&call);
  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - duplication without a source directory\n");
}

static void
test_delete_metadata_only (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Kept", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvasha_instance_manager_delete_async (
     manager, instance, FALSE, delete_finished, &call);
  async_call_run (&call);
  g_assert_no_error (call.error);
  g_assert_true (call.success);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

  g_autoptr (NuvashaInstanceManager) fresh = nuvasha_instance_manager_new (data_dir);
  g_assert_true (nuvasha_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 0);

  async_call_clear (&call);
  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - delete keeps the directory on request\n");
}

static void
test_delete_with_directory (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Gone", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_autofree char *level_path =
    g_build_filename (directory, "saves", "level.dat", NULL);
  write_file (level_path, "data");
  g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvasha_instance_manager_delete_async (
     manager, instance, TRUE, delete_finished, &call);
  async_call_run (&call);
  g_assert_no_error (call.error);
  g_assert_true (call.success);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  g_assert_false (g_file_test (directory, G_FILE_TEST_EXISTS));

  g_autoptr (NuvashaInstanceManager) fresh = nuvasha_instance_manager_new (data_dir);
  g_assert_true (nuvasha_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 0);

  async_call_clear (&call);
  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - delete removes the directory\n");
}

static void
test_delete_unregistered (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  g_autoptr (NuvashaInstance) stranger =
    nuvasha_instance_new (NULL, "Stranger", "/tmp/nuvasha-test-stranger", "1.21.1",
                      nuvasha_LOADER_VANILLA, &error);
  g_assert_no_error (error);
  g_assert_nonnull (stranger);

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvasha_instance_manager_delete_async (
     manager, stranger, FALSE, delete_finished, &call);
  async_call_run (&call);
  g_assert_error (call.error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_NOT_FOUND);
  g_assert_false (call.success);

  async_call_clear (&call);
  remove_recursive (data_dir);

  g_print ("ok - delete of an unregistered instance\n");
}

static void
test_open_folder_without_directory (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Folderless", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_autofree char *icon = g_build_filename (directory, "icon.png", NULL);
  g_assert_true (nuvasha_fs_unlink (icon, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_rmdir (directory), ==, 0);

  /* The directory is gone, so nothing is launched: the test fails
   * with an error instead of needing a file manager. */
  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

  call.operation =
    nuvasha_instance_manager_open_folder_async (manager, instance,
                                            open_folder_finished, &call);
  async_call_run (&call);
  g_assert_error (call.error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_assert_false (call.success);

  async_call_clear (&call);
  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - open folder of a missing directory\n");
}

static void
test_change_directory_validation (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *first =
    nuvasha_instance_manager_create (manager, "One", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);

  NuvashaInstance *second =
    nuvasha_instance_manager_create (manager, "Two", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (second);

  g_autofree char *first_directory = prop_string (first, "directory");
  g_autofree char *second_directory = prop_string (second, "directory");
  g_autofree char *elsewhere_parent =
    g_build_filename (data_dir, "elsewhere", NULL);
  g_autofree char *elsewhere =
    g_build_filename (elsewhere_parent, "moved", NULL);

  g_assert_cmpint (g_mkdir_with_parents (elsewhere_parent, 0755), ==, 0);

  /* A path that does not exist yet and whose parent does is accepted. */
  g_autofree char *canonical = NULL;
  g_assert_true (nuvasha_instance_manager_check_new_directory (
    manager, first, elsewhere, &canonical, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (canonical, ==, elsewhere);

  /* A relative path is not. */
  g_assert_false (nuvasha_instance_manager_check_new_directory (
    manager, first, "relative/path", NULL, &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  {
    g_autofree char *escape =
      g_build_filename (data_dir, "elsewhere", "..", "climb", NULL);

    g_assert_false (nuvasha_instance_manager_check_new_directory (
      manager, first, escape, NULL, &error));
    g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                    nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);
  }

  /* The filesystem root is not. */
  g_assert_false (nuvasha_instance_manager_check_new_directory (
    manager, first, G_DIR_SEPARATOR_S, NULL, &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* The directory the instance already has is not a change. */
  g_assert_false (nuvasha_instance_manager_check_new_directory (
    manager, first, first_directory, NULL, &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* The directory of another instance is not, neither is one inside it
     or one that would contain it. */
  g_assert_false (nuvasha_instance_manager_check_new_directory (
    manager, first, second_directory, NULL, &error));
  g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  {
    g_autofree char *inside =
      g_build_filename (second_directory, "nested", NULL);

    g_assert_false (nuvasha_instance_manager_check_new_directory (
      manager, first, inside, NULL, &error));
    g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                    nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);
  }

  {
    g_autofree char *parent = g_path_get_dirname (second_directory);

    g_assert_false (nuvasha_instance_manager_check_new_directory (
      manager, first, parent, NULL, &error));
    g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                    nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);
  }

  /* A path that exists already is not taken over. */
  {
    g_autofree char *taken =
      g_build_filename (elsewhere_parent, "taken", NULL);

    write_file (taken, "not an instance");

    g_assert_false (nuvasha_instance_manager_check_new_directory (
      manager, first, taken, NULL, &error));
    g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                    nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);

    {
      g_autofree char *contents = read_file (taken);

      g_assert_cmpstr (contents, ==, "not an instance");
    }
  }

  /* A path whose parent does not exist is refused before a copy is
     started, not halfway through one. */
  {
    g_autofree char *orphan =
      g_build_filename (data_dir, "no-such-parent", "child", NULL);

    g_assert_false (nuvasha_instance_manager_check_new_directory (
      manager, first, orphan, NULL, &error));
    g_assert_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                    nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);
    g_assert_false (g_file_test (orphan, G_FILE_TEST_EXISTS));
  }

  /* Nothing above changed an instance or wrote anything. */
  {
    g_autofree char *stored = prop_string (first, "directory");

    g_assert_cmpstr (stored, ==, first_directory);
  }

  g_assert_cmpuint (manager_count (manager), ==, 2);

  g_object_unref (first);
  g_object_unref (second);
  remove_recursive (data_dir);

  g_print ("ok - the rules for a new directory\n");
}

static void
test_change_directory_moves_files (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Mover", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *old_directory = prop_string (instance, "directory");
  g_autofree char *save_path = g_build_filename (old_directory, "world",
                                                 "level.dat", NULL);
  g_autofree char *mods_path = g_build_filename (old_directory, "mods",
                                                 "one.jar", NULL);

  write_file (save_path, "the world");
  write_file (mods_path, "a mod");

  g_autofree char *parent =
    g_build_filename (data_dir, "elsewhere", NULL);
  g_autofree char *new_directory =
    g_build_filename (parent, "moved", NULL);

  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvasha_instance_manager_change_directory_async (
     manager, instance, new_directory, change_directory_finished, &call);
  async_call_run (&call);

  g_assert_no_error (call.error);
  g_assert_true (call.success);
  async_call_clear (&call);

  /* The stored path is the new one. */
  {
    g_autofree char *stored = prop_string (instance, "directory");

    g_assert_cmpstr (stored, ==, new_directory);
  }

  /* The files came with it. */
  {
    g_autofree char *level = g_build_filename (new_directory, "world",
                                               "level.dat", NULL);
    g_autofree char *jar = g_build_filename (new_directory, "mods",
                                             "one.jar", NULL);

    {
      g_autofree char *world = read_file (level);
      g_autofree char *mod = read_file (jar);

      g_assert_cmpstr (world, ==, "the world");
      g_assert_cmpstr (mod, ==, "a mod");
    }
  }

  /* The old directory is gone, so the instance exists in one place. */
  g_assert_false (g_file_test (old_directory, G_FILE_TEST_EXISTS));

  /* And the file that records it says the new path, not the old one. */
  {
    g_autofree char *metadata_path =
      g_build_filename (data_dir, "instances.json", NULL);
    g_autofree char *metadata = read_file (metadata_path);

    g_assert_nonnull (strstr (metadata, new_directory));
    g_assert_null (strstr (metadata, old_directory));
  }

  /* A manager that reads the file back agrees. */
  {
    g_autoptr (NuvashaInstanceManager) reloaded =
      nuvasha_instance_manager_new (data_dir);

    g_assert_true (nuvasha_instance_manager_load (reloaded, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (manager_count (reloaded), ==, 1);
  }

  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - changing the directory moves the files\n");
}

static void
test_change_directory_failure_keeps_everything (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (data_dir);

  NuvashaInstance *instance =
    nuvasha_instance_manager_create (manager, "Keeper", NULL, "1.21.1",
                                 nuvasha_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *old_directory = prop_string (instance, "directory");
  g_autofree char *level =
    g_build_filename (old_directory, "world", "level.dat", NULL);

  write_file (level, "the world");

  /* A destination that cannot be written to: the copy fails on its first
     file, which is as early as a failure can happen. */
  g_autofree char *parent = g_build_filename (data_dir, "locked", NULL);
  g_autofree char *new_directory =
    g_build_filename (parent, "moved", NULL);

  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);
  g_assert_cmpint (g_chmod (parent, 0500), ==, 0);

  if (geteuid () == 0)
    {
      /* Root writes through a read-only directory, so this case cannot be
         made here; what it would test is already covered above. */
      g_assert_cmpint (g_chmod (parent, 0755), ==, 0);
      g_object_unref (instance);
      remove_recursive (data_dir);
      g_print ("ok - a failed move keeps the old directory and metadata\n");
      return;
    }

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvasha_instance_manager_change_directory_async (
     manager, instance, new_directory, change_directory_finished, &call);
  async_call_run (&call);

  g_assert_nonnull (call.error);
  g_assert_false (call.success);
  async_call_clear (&call);

  /* The instance still points where it did, and its files are there. */
  {
    g_autofree char *stored = prop_string (instance, "directory");
    g_autofree char *world = read_file (level);

    g_assert_cmpstr (stored, ==, old_directory);
    g_assert_cmpstr (world, ==, "the world");
  }

  /* The metadata on disk never learned about the new path. */
  {
    g_autofree char *metadata_path =
      g_build_filename (data_dir, "instances.json", NULL);
    g_autofree char *metadata = read_file (metadata_path);

    g_assert_null (strstr (metadata, new_directory));
    g_assert_nonnull (strstr (metadata, old_directory));
  }

  g_assert_false (g_file_test (new_directory, G_FILE_TEST_EXISTS));

  g_assert_cmpint (g_chmod (parent, 0755), ==, 0);
  g_object_unref (instance);
  remove_recursive (data_dir);

  g_print ("ok - a failed move keeps the old directory and metadata\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  test_create_and_persist ();
  test_create_explicit_directory ();
  test_create_sanitized_name ();
  test_create_duplicate_names ();
  test_path_validation ();
  test_load_missing_file ();
  test_load_removes_missing_directory ();
  test_corrupt_file ();
  test_missing_field_file ();
  test_version_mismatch ();
  test_atomic_save_failure ();
  test_rename ();
  test_model_updates_are_atomic ();
  test_duplicate ();
  test_duplicate_without_directory ();
  test_delete_metadata_only ();
  test_delete_with_directory ();
  test_delete_unregistered ();
  test_open_folder_without_directory ();
  test_change_directory_validation ();
  test_change_directory_moves_files ();
  test_change_directory_failure_keeps_everything ();

  return EXIT_SUCCESS;
}
