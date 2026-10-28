#include "test-instance-manager-common.h"

static void
test_create_and_persist (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);
  g_assert_nonnull (nuvashalauncher_instance_manager_get_instances (manager));
  g_assert_cmpuint (manager_count (manager), ==, 0);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Pack One", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_FABRIC, "0.16.9", &error);
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
  g_autoptr (nuvashalauncherInstanceManager) fresh = nuvashalauncher_instance_manager_new (data_dir);
  g_assert_true (nuvashalauncher_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 1);

  g_autoptr (nuvashalauncherInstance) loaded =
    g_list_model_get_item (
      G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (fresh)), 0);
  g_assert_cmpstr (nuvashalauncher_instance_get_id (loaded), ==,
                   nuvashalauncher_instance_get_id (instance));

  g_autofree char *loaded_name = prop_string (loaded, "name");
  g_autofree char *loaded_version = prop_string (loaded, "minecraft-version");
  g_autofree char *loaded_loader_version =
    prop_string (loaded, "loader-version");
  g_autofree char *loaded_directory = prop_string (loaded, "directory");

  g_assert_cmpstr (loaded_name, ==, "Pack One");
  g_assert_cmpstr (loaded_version, ==, "1.21.1");
  g_assert_cmpstr (loaded_loader_version, ==, "0.16.9");
  g_assert_cmpstr (loaded_directory, ==, directory);

  g_assert_true (nuvashalauncher_instance_manager_get_by_id (
    fresh, nuvashalauncher_instance_get_id (instance)) == loaded);

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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Chosen", chosen, "1.20.4",
                                 nuvashalauncher_LOADER_QUILT, NULL, &error);
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  /* A name that cannot be used as a directory component is rejected rather
   * than silently changed. */
  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "../../evil", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
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
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *first =
    nuvashalauncher_instance_manager_create (manager, "test", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);

  nuvashalauncherInstance *second =
    nuvashalauncher_instance_manager_create (manager, "test", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (second);

  nuvashalauncherInstance *third =
    nuvashalauncher_instance_manager_create (manager, "test", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *first =
    nuvashalauncher_instance_manager_create (manager, "One", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);

  g_autofree char *first_directory = prop_string (first, "directory");

  /* A relative path is refused. */
  g_autoptr (nuvashalauncherInstance) relative =
    nuvashalauncher_instance_manager_create (manager, "Two", "relative/path", "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_null (relative);
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* A path with ".." inside is refused and never created. */
  g_autofree char *escape =
    g_build_filename (data_dir, "instances", "..", "escape", NULL);
  g_autoptr (nuvashalauncherInstance) escaping =
    nuvashalauncher_instance_manager_create (manager, "Three", escape, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_null (escaping);
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  g_autofree char *escape_canonical = g_canonicalize_filename (escape, NULL);
  g_assert_false (g_file_test (escape_canonical, G_FILE_TEST_EXISTS));

  /* The filesystem root is refused. */
  g_autoptr (nuvashalauncherInstance) root =
    nuvashalauncher_instance_manager_create (manager, "Four", G_DIR_SEPARATOR_S, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_null (root);
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* The directory of an existing instance is refused. */
  g_autoptr (nuvashalauncherInstance) same =
    nuvashalauncher_instance_manager_create (manager, "Five", first_directory, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_null (same);
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* A directory containing an existing instance is refused. */
  g_autofree char *parent = g_path_get_dirname (first_directory);
  g_autoptr (nuvashalauncherInstance) containing =
    nuvashalauncher_instance_manager_create (manager, "Six", parent, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_null (containing);
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* A directory inside an existing instance is refused too. */
  g_autofree char *inside =
    g_build_filename (first_directory, "nested", NULL);
  g_autoptr (nuvashalauncherInstance) nested =
    nuvashalauncher_instance_manager_create (manager, "Seven", inside, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_null (nested);
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  /* First run: no file at all is a success with an empty list. */
  g_assert_true (nuvashalauncher_instance_manager_load (manager, &error));
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
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Removed", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_autofree char *directory = prop_string (instance, "directory");
  g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

  remove_recursive (directory);

  g_autoptr (nuvashalauncherInstanceManager) fresh =
    nuvashalauncher_instance_manager_new (data_dir);
  g_assert_true (nuvashalauncher_instance_manager_load (fresh, &error));
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
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  g_assert_false (nuvashalauncher_instance_manager_load (manager, &error));
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
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  /* An entry without the required fields is a corrupt file, reported
   * as an error instead of crashing. */
  g_assert_false (nuvashalauncher_instance_manager_load (manager, &error));
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
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  g_assert_false (nuvashalauncher_instance_manager_load (manager, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_CORRUPT);
  g_clear_error (&error);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  assert_quarantined (data_dir, text);

  remove_recursive (data_dir);

  g_print ("ok - unsupported metadata version\n");
}

static void
test_duplicate_metadata_entries_are_quarantined (void)
{
  const char *ids[] = {
    "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"name\":\"One\","
    "\"directory\":\"/tmp/one\",\"minecraft_version\":\"1.21.1\",\"loader\":\"vanilla\"}",
    "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"name\":\"Two\","
    "\"directory\":\"/tmp/two\",\"minecraft_version\":\"1.21.1\",\"loader\":\"vanilla\"}"
  };
  const char *names[] = {
    "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"name\":\"Pack\","
    "\"directory\":\"/tmp/one\",\"minecraft_version\":\"1.21.1\",\"loader\":\"vanilla\"}",
    "{\"id\":\"66666666-7777-8888-9999-000000000000\",\"name\":\" pack \","
    "\"directory\":\"/tmp/two\",\"minecraft_version\":\"1.21.1\",\"loader\":\"vanilla\"}"
  };

  for (guint case_index = 0; case_index < 2; case_index++)
    {
      g_autofree char *data_dir = make_tmp_dir ();
      g_autofree char *metadata_path =
        g_build_filename (data_dir, "instances.json", NULL);
      g_autofree char *text = g_strdup_printf (
        "{\"version\":2,\"instances\":[%s,%s]}",
        case_index == 0 ? ids[0] : names[0],
        case_index == 0 ? ids[1] : names[1]);
      g_autoptr (GError) error = NULL;
      g_autoptr (nuvashalauncherInstanceManager) manager =
        nuvashalauncher_instance_manager_new (data_dir);

      write_file (metadata_path, text);
      g_assert_false (nuvashalauncher_instance_manager_load (manager, &error));
      g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                      nuvashalauncher_INSTANCE_MANAGER_ERROR_CORRUPT);
      g_clear_error (&error);
      g_assert_cmpuint (manager_count (manager), ==, 0);
      assert_quarantined (data_dir, text);
      remove_recursive (data_dir);
    }

  g_print ("ok - duplicate ids and normalized names quarantine metadata\n");
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Durable", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
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

  g_assert_false (nuvashalauncher_instance_manager_save (manager, &error));
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



int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
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
  test_duplicate_metadata_entries_are_quarantined ();
  test_atomic_save_failure ();
  return 0;
}
