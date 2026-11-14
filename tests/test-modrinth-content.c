#include "test-modrinth-common.h"

static void
test_dependencies_of_a_version (void)
{
  g_autofree char *json =
    read_fixture ("modrinth-version-dependencies.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherModrinthVersion) version = NULL;
  g_autoptr (GPtrArray) pending = NULL;
  g_autoptr (GPtrArray) conflicts = NULL;

  version = nuvashalauncher_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (version->dependencies->len, ==, 1);

  pending = nuvashalauncher_modrinth_resolve_install (version, NULL, &conflicts, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (pending->len, ==, 1);
  g_assert_cmpuint (conflicts->len, ==, 0);

  {
    NuvashaLauncherModrinthDependency *dependency = g_ptr_array_index (pending, 0);

    g_assert_cmpint (dependency->type, ==, nuvashalauncher_DEPENDENCY_REQUIRED);
    g_assert_cmpstr (dependency->project_id, ==, "AANobbMI");
    g_assert_cmpstr (dependency->version_id, ==, "ryOMVRuG");
  }

  /* With Sodium already in the instance there is nothing left to fetch,
   * which is how an install avoids a second copy of a dependency. */
  {
    g_autoptr (GHashTable) installed =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr (GPtrArray) again = NULL;

    g_hash_table_add (installed, g_strdup ("AANobbMI"));
    again = nuvashalauncher_modrinth_resolve_install (version, installed, NULL, &error);

    g_assert_no_error (error);
    g_assert_cmpuint (again->len, ==, 0);
  }

  g_print ("ok - a required dependency is fetched once, and not at all "
           "when it is there\n");
}

static void
test_dependency_kinds (void)
{
  g_autofree char *json =
    read_fixture ("modrinth-version-dependency-kinds.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherModrinthVersion) version = NULL;
  g_autoptr (GPtrArray) pending = NULL;
  g_autoptr (GPtrArray) conflicts = NULL;
  g_autoptr (GHashTable) installed =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  version = nuvashalauncher_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (version->dependencies->len, ==, 5);

  /* Embeddium's conflicting project is in the instance, so the install
   * has to say so before anything is written. */
  g_hash_table_add (installed, g_strdup ("4ZqxOvjD"));

  pending = nuvashalauncher_modrinth_resolve_install (version, installed, &conflicts,
                                          &error);
  g_assert_no_error (error);

  /* Only what the version cannot run without: the optional and the
   * embedded ones are not fetched, and the duplicate of the required
   * dependency is one file, not two. */
  g_assert_cmpuint (pending->len, ==, 1);
  g_assert_cmpstr (((NuvashaLauncherModrinthDependency *) g_ptr_array_index (pending, 0))
                       ->project_id,
                   ==, "AANobbMI");

  g_assert_cmpuint (conflicts->len, ==, 1);
  g_assert_cmpstr (((NuvashaLauncherModrinthDependency *) g_ptr_array_index (conflicts, 0))
                       ->project_id,
                   ==, "4ZqxOvjD");
  g_assert_cmpstr (nuvashalauncher_modrinth_dependency_to_string (
                       ((NuvashaLauncherModrinthDependency *)
                            g_ptr_array_index (conflicts, 0))
                           ->type),
                   ==, "incompatible");

  g_print ("ok - embedded and optional are left alone, a conflict is "
           "reported\n");
}

static void
test_dependency_without_project (void)
{
  const char *json =
    "{\"id\":\"broken\",\"project_id\":\"p\",\"name\":\"n\","
    "\"version_number\":\"1\",\"date_published\":\"2025-01-01T00:00:00Z\","
    "\"game_versions\":[\"1.20.1\"],\"loaders\":[\"fabric\"],"
    "\"files\":[{\"filename\":\"a.jar\",\"url\":\"https://example.invalid/a.jar\","
    "\"hashes\":{\"sha1\":\"aa\"},\"primary\":true,\"size\":1}],"
    "\"dependencies\":[{\"version_id\":null,\"project_id\":null,"
    "\"file_name\":\"only-a-name.jar\",\"dependency_type\":\"required\"}]}";
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherModrinthVersion) version = NULL;
  GPtrArray *pending = NULL;

  version = nuvashalauncher_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);

  /* A dependency that names no project names nothing that could be
   * fetched, and the install says so instead of going on without it. */
  pending = nuvashalauncher_modrinth_resolve_install (version, NULL, NULL, &error);
  g_assert_null (pending);
  g_assert_error (error, nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_PARSE);

  g_print ("ok - a dependency that cannot be fetched stops the install\n");
}

static void
test_content_types (void)
{
  g_assert_cmpstr (nuvashalauncher_content_type_to_string (nuvashalauncher_CONTENT_MOD), ==, "mod");
  g_assert_cmpstr (nuvashalauncher_content_type_to_string (nuvashalauncher_CONTENT_RESOURCE_PACK), ==,
                   "resourcepack");
  g_assert_cmpstr (nuvashalauncher_content_type_to_string (nuvashalauncher_CONTENT_SHADER_PACK), ==,
                   "shader");
  g_assert_cmpstr (nuvashalauncher_content_type_to_string (nuvashalauncher_CONTENT_DATAPACK), ==,
                   "datapack");

  g_assert_cmpstr (nuvashalauncher_content_type_folder (nuvashalauncher_CONTENT_MOD), ==, "mods");
  g_assert_cmpstr (nuvashalauncher_content_type_folder (nuvashalauncher_CONTENT_RESOURCE_PACK), ==,
                   "resourcepacks");
  g_assert_cmpstr (nuvashalauncher_content_type_folder (nuvashalauncher_CONTENT_SHADER_PACK), ==,
                   "shaderpacks");
  g_assert_cmpstr (nuvashalauncher_content_type_folder (nuvashalauncher_CONTENT_DATAPACK), ==,
                   "datapacks");

  g_assert_cmpstr (nuvashalauncher_content_type_extension (nuvashalauncher_CONTENT_MOD), ==, ".jar");
  g_assert_cmpstr (nuvashalauncher_content_type_extension (nuvashalauncher_CONTENT_RESOURCE_PACK), ==,
                   ".zip");
  g_assert_cmpstr (nuvashalauncher_content_type_extension (nuvashalauncher_CONTENT_DATAPACK), ==,
                   ".zip");

  /* Only a mod needs a loader, which makes all content pages share the same
   * page implementation with a different content type. */
  g_assert_true (nuvashalauncher_content_type_needs_loader (nuvashalauncher_CONTENT_MOD));
  g_assert_false (nuvashalauncher_content_type_needs_loader (nuvashalauncher_CONTENT_RESOURCE_PACK));
  g_assert_false (nuvashalauncher_content_type_needs_loader (nuvashalauncher_CONTENT_SHADER_PACK));
  g_assert_false (nuvashalauncher_content_type_needs_loader (nuvashalauncher_CONTENT_DATAPACK));

  g_print ("ok - the content kinds name their own folders and loaders\n");
}

static void
test_folder_and_disabled_names (void)
{
  g_autofree char *mods = NULL;
  g_autofree char *packs = NULL;
  g_autofree char *disabled = NULL;
  g_autofree char *enabled = NULL;
  g_autofree char *again = NULL;

  mods = nuvashalauncher_content_folder_path ("/tmp/instance", nuvashalauncher_CONTENT_MOD);
  packs = nuvashalauncher_content_folder_path ("/tmp/instance",
                                   nuvashalauncher_CONTENT_RESOURCE_PACK);
  g_assert_cmpstr (mods, ==, "/tmp/instance/mods");
  g_assert_cmpstr (packs, ==, "/tmp/instance/resourcepacks");

  /* Disabling puts the suffix on the name, which is what the loaders and
   * the game stop loading; enabling takes it off again. */
  disabled = nuvashalauncher_content_disabled_name ("sodium.jar", FALSE);
  g_assert_cmpstr (disabled, ==, "sodium.jar.disabled");

  enabled = nuvashalauncher_content_disabled_name (disabled, TRUE);
  g_assert_cmpstr (enabled, ==, "sodium.jar");

  /* A name that carries it twice does not get it twice. */
  again = nuvashalauncher_content_disabled_name (disabled, FALSE);
  g_assert_cmpstr (again, ==, "sodium.jar.disabled");

  g_print ("ok - a disabled file is the same file with a suffix on\n");
}

static void
test_scan_and_toggle (void)
{
  g_autofree char *instance = NULL;
  g_autofree char *folder_path = NULL;
  g_autoptr (GPtrArray) lithium = NULL;
  g_autofree char *renamed = NULL;
  g_autofree char *back = NULL;
  g_autofree char *same = NULL;
  g_autoptr (GFile) folder = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;

  instance = g_dir_make_tmp ("nuvashalauncher-content-XXXXXX", NULL);
  g_assert_nonnull (instance);

  folder_path = nuvashalauncher_content_folder_path (instance, nuvashalauncher_CONTENT_MOD);
  folder = g_file_new_for_path (folder_path);
  g_assert_true (g_file_make_directory_with_parents (folder, NULL, &error));
  g_assert_no_error (error);

  /* One mod, one mod that was disabled, and a file that is not a mod. */
  file = g_file_get_child (folder, "sodium-fabric.jar");
  g_assert_true (g_file_replace_contents (file, "a", 1, NULL, FALSE,
                                          G_FILE_CREATE_NONE, NULL, NULL,
                                          &error));
  g_clear_object (&file);
  file = g_file_get_child (folder, "lithium.jar.disabled");
  g_assert_true (g_file_replace_contents (file, "b", 1, NULL, FALSE,
                                          G_FILE_CREATE_NONE, NULL, NULL,
                                          &error));
  g_clear_object (&file);
  file = g_file_get_child (folder, "notes.txt");
  g_assert_true (g_file_replace_contents (file, "c", 1, NULL, FALSE,
                                          G_FILE_CREATE_NONE, NULL, NULL,
                                          &error));
  g_assert_no_error (error);

  lithium = scan (instance, nuvashalauncher_CONTENT_MOD);

  /* Only the mods are listed, and both are: a disabled one is still in
     the folder and the user has to see it to enable it again. */
  g_assert_nonnull (lithium);
  g_assert_cmpuint (lithium->len, ==, 2);

  {
    NuvashaLauncherContentFile *disabled = g_ptr_array_index (lithium, 0);
    NuvashaLauncherContentFile *enabled = g_ptr_array_index (lithium, 1);

    /* Sorted by name, and the suffix is not part of the name a user
       reads. */
    g_assert_cmpstr (disabled->filename, ==, "lithium.jar");
    g_assert_false (disabled->enabled);
    g_assert_cmpstr (disabled->sha1, ==,
                     "e9d71f5ee7c92d6dc9e92ffdad17b8bd49418f98");

    g_assert_cmpstr (enabled->filename, ==, "sodium-fabric.jar");
    g_assert_true (enabled->enabled);
    g_assert_cmpstr (enabled->sha1, ==,
                     "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8");
  }

  /* Enabling it takes the suffix off, and disabling it puts it back. */
  {
    const char *disabled_path =
      ((NuvashaLauncherContentFile *) g_ptr_array_index (lithium, 0))->path;
    g_autofree char *enabled_path =
      g_build_filename (folder_path, "lithium.jar", NULL);

    /* A disabled file is loaded again by taking the suffix off. */
    g_free (back);
    back = nuvashalauncher_content_set_disabled (disabled_path, FALSE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (back, ==, enabled_path);
    g_assert_true (g_file_test (back, G_FILE_TEST_EXISTS));

    /* Asking for the state a file is already in is not a rename. */
    same = nuvashalauncher_content_set_disabled (back, FALSE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (same, ==, back);
    g_assert_true (g_file_test (same, G_FILE_TEST_EXISTS));

    /* And it goes back to being a file the loader skips. */
    renamed = nuvashalauncher_content_set_disabled (back, TRUE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (renamed, ==, disabled_path);
    g_assert_true (g_file_test (renamed, G_FILE_TEST_EXISTS));
  }

  /* The folder is read again with the file disabled under its new name,
     and the resource pack folder of an instance that has none is
     simply empty. */
  {
    g_autoptr (GPtrArray) again = scan (instance, nuvashalauncher_CONTENT_MOD);

    g_assert_cmpuint (again->len, ==, 2);
    g_assert_false (((NuvashaLauncherContentFile *) g_ptr_array_index (again, 0))->enabled);
  }

  {
    g_autoptr (GPtrArray) none = scan (instance, nuvashalauncher_CONTENT_SHADER_PACK);

    /* A folder that does not exist holds no files, which is what an
       instance that never installed a shader pack looks like. */
    g_assert_nonnull (none);
    g_assert_cmpuint (none->len, ==, 0);
  }

  g_print ("ok - the folder is read, and enable and disable rename it\n");
}

typedef struct
{
  GMainLoop *loop;
  gboolean success;
  GError *error;
} ContentMetaWait;

static void
on_content_metadata_finished (GObject *source G_GNUC_UNUSED,
                              GAsyncResult *result,
                              gpointer user_data)
{
  ContentMetaWait *wait = user_data;
  wait->success = nuvashalauncher_content_metadata_finish (result, &wait->error);
  g_main_loop_quit (wait->loop);
}

static void
test_content_metadata_and_helpers (void)
{
  /* Test folder creation and file operations */
  g_autofree char *dir = g_dir_make_tmp ("nuvashalauncher-test-cf-XXXXXX", NULL);
  g_assert_nonnull (dir);
  g_autofree char *sub = g_build_filename (dir, "subfolder", NULL);
  g_autoptr (GError) error = NULL;

  g_assert_true (nuvashalauncher_content_ensure_folder (sub, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (sub, G_FILE_TEST_IS_DIR));

  g_autofree char *dummy_file = g_build_filename (sub, "test.txt", NULL);
  g_assert_true (g_file_set_contents (dummy_file, "hello", 5, &error));
  g_assert_no_error (error);

  g_assert_true (nuvashalauncher_content_remove_file (dummy_file, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (dummy_file, G_FILE_TEST_EXISTS));

  /* Test nuvashalauncher_content_file_refresh_search_key */
  NuvashaLauncherContentFile cf = { 0 };
  cf.filename = (char *) "mod-file.jar";
  cf.project_name = (char *) "My Amazing Mod";
  nuvashalauncher_content_file_refresh_search_key (&cf);
  g_assert_nonnull (cf.search_key);
  g_assert_true (strstr (cf.search_key, "amazing") != NULL);
  g_free (cf.search_key);

  /* Test async metadata extraction with empty and non-empty file lists */
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_content_file_free);
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  ContentMetaWait wait1 = { loop, FALSE, NULL };

  /* Empty list finishes immediately */
  NuvashaLauncherTaskScope *scope1 = nuvashalauncher_content_metadata_async (files, nuvashalauncher_CONTENT_MOD, on_content_metadata_finished, &wait1);
  g_assert_nonnull (scope1);
  g_main_loop_run (loop);
  g_assert_true (wait1.success);
  g_assert_no_error (wait1.error);
  nuvashalauncher_task_scope_unref (scope1);

  /* List with a file finishes through worker pool */
  NuvashaLauncherContentFile *file_item = g_new0 (NuvashaLauncherContentFile, 1);
  file_item->path = g_strdup (sub);
  file_item->filename = g_strdup ("nonexistent.jar");
  g_ptr_array_add (files, file_item);

  ContentMetaWait wait2 = { loop, FALSE, NULL };
  NuvashaLauncherTaskScope *scope2 = nuvashalauncher_content_metadata_async (files, nuvashalauncher_CONTENT_MOD, on_content_metadata_finished, &wait2);
  g_assert_nonnull (scope2);
  g_main_loop_run (loop);
  g_assert_true (wait2.success);
  g_assert_no_error (wait2.error);
  nuvashalauncher_task_scope_unref (scope2);

  g_main_loop_unref (loop);

  g_autoptr (GFile) dir_gfile = g_file_new_for_path (dir);
  g_autoptr (GFile) sub_gfile = g_file_new_for_path (sub);
  g_file_delete (sub_gfile, NULL, NULL);
  g_file_delete (dir_gfile, NULL, NULL);

  g_print ("ok - content file helpers and metadata tasks succeed\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/modrinth/dependencies_of_a_version", test_dependencies_of_a_version);
  g_test_add_func ("/modrinth/dependency_kinds", test_dependency_kinds);
  g_test_add_func ("/modrinth/dependency_without_project", test_dependency_without_project);
  g_test_add_func ("/modrinth/content_types", test_content_types);
  g_test_add_func ("/modrinth/folder_and_disabled_names", test_folder_and_disabled_names);
  g_test_add_func ("/modrinth/scan_and_toggle", test_scan_and_toggle);
  g_test_add_func ("/modrinth/metadata_and_helpers", test_content_metadata_and_helpers);
  return g_test_run ();
}
