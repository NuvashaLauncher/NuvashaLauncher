#include "test-modpack-common.h"

static void
test_install_offline (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *index_json =
    pack_index ("Offline Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autofree char *instances_dir = g_build_filename (data_dir, "instances", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;
  g_autofree char *instance_dir = NULL;
  g_autofree char *name = NULL;
  g_autofree char *minecraft = NULL;
  g_autofree char *icon = NULL;
  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;
  g_autofree char *options = NULL;
  g_autofree char *client = NULL;
  g_autofree char *server = NULL;

  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
    ZIP_FILE ("overrides/config/options.txt", "from overrides"),
    ZIP_FILE ("overrides/config/mods.toml", "only overrides"),
    ZIP_FILE ("overrides/icon.png", "icon bytes"),
    ZIP_FILE ("overrides/mods/.index/index.json", "internal index"),
    ZIP_FILE ("client-overrides/config/options.txt", "from client"),
    ZIP_FILE ("server-overrides/config/server.txt", "from server"),
  };

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));

  manager = manager_new (data_dir);

  pack = nuvashalauncher_modpack_new_from_zip (zip_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_cmpstr (pack->archive_path, ==, zip_path);
  g_assert_true (nuvashalauncher_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  /* No directory of its own: the manager's default for the name. */
  instance = install_now (pack, manager, "Offline Pack", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_object_get (instance, "directory", &instance_dir, "name", &name,
                "minecraft-version", &minecraft, "loader", &loader,
                "icon", &icon, NULL);
  g_assert_cmpstr (name, ==, "Offline Pack");
  g_assert_cmpstr (minecraft, ==, "1.20.1");
  g_assert_cmpint (loader, ==, nuvashalauncher_LOADER_VANILLA);
  g_assert_true (g_str_has_prefix (instance_dir, instances_dir));
  g_assert_true (g_file_test (instance_dir, G_FILE_TEST_IS_DIR));
  g_assert_nonnull (icon);
  {
    g_autofree char *icon_name = g_path_get_basename (icon);
    g_autofree char *icon_data = read_file (icon);

    g_assert_cmpstr (icon_name, ==, "icon.png");
    g_assert_cmpstr (icon_data, ==, "icon bytes");
  }
  {
    g_autofree char *imported_index =
      g_build_filename (instance_dir, "mods", ".index", "index.json", NULL);

    g_assert_false (g_file_test (imported_index, G_FILE_TEST_EXISTS));
  }

  {
    g_autofree char *options_path =
      g_build_filename (instance_dir, "config", "options.txt", NULL);
    g_autofree char *mods_path =
      g_build_filename (instance_dir, "config", "mods.toml", NULL);
    g_autofree char *server_path =
      g_build_filename (instance_dir, "config", "server.txt", NULL);

    options = read_file (options_path);
    g_assert_cmpstr (options, ==, "from client");

    client = read_file (mods_path);
    g_assert_cmpstr (client, ==, "only overrides");

    server = g_steal_pointer (&server_path);
    g_assert_false (g_file_test (server, G_FILE_TEST_EXISTS));
  }

  /* The instance is registered, and no temporary directory is left
   * next to it. */
  g_assert_cmpuint (manager_count (manager), ==, 1);
  g_assert_false (has_temp_dir (instances_dir));

  remove_recursive (dir);
  g_print ("ok - a pack with no files installs from its overrides\n");
}

static void
test_install_rolls_back (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *target =
    g_build_filename (data_dir, "instances", "rollback", NULL);
  g_autofree char *parent = g_path_get_dirname (target);
  g_autofree char *files_json = g_strdup_printf (
    "{\"path\":\"mods/x.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"fileSize\":10,"
    "\"downloads\":[\"https://evil.example/x.jar\"]}");
  g_autofree char *index_json =
    pack_index ("Bad Pack", files_json, "\"minecraft\":\"1.20.1\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;

  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
  };

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));

  manager = manager_new (data_dir);

  pack = nuvashalauncher_modpack_new_from_zip (zip_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvashalauncher_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  instance = install_now (pack, manager, "Bad Pack", target, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_URL);
  g_clear_error (&error);

  /* Nothing of the pack is where it wanted to go, nothing of it is
   * next to where it wanted to go, and nothing lists an instance. */
  g_assert_false (g_file_test (target, G_FILE_TEST_EXISTS));
  g_assert_false (has_temp_dir (parent));
  g_assert_cmpuint (manager_count (manager), ==, 0);

  remove_recursive (dir);
  g_print ("ok - a pack whose URLs are refused leaves nothing behind\n");
}

static void
test_install_leaves_user_data (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *target =
    g_build_filename (data_dir, "instances", "taken", NULL);
  g_autofree char *parent = g_path_get_dirname (target);
  g_autofree char *world = g_build_filename (target, "world", "level.dat", NULL);
  g_autofree char *index_json =
    pack_index ("Taken Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;

  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
  };

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));

  /* What is in the place already is user data, not ours to touch. */
  g_autofree char *world_dir = g_path_get_dirname (world);

  g_assert_cmpint (g_mkdir_with_parents (world_dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (world, "precious", -1, &error));
  g_assert_no_error (error);

  manager = manager_new (data_dir);

  pack = nuvashalauncher_modpack_new_from_zip (zip_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvashalauncher_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  instance = install_now (pack, manager, "Taken Pack", target, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_IO);
  g_clear_error (&error);

  /* The user data is as it was, and no temporary directory was even
   * made: the refusal happens in front of any tree. */
  g_autofree char *kept = read_file (world);

  g_assert_cmpstr (kept, ==, "precious");
  g_assert_false (has_temp_dir (parent));
  g_assert_cmpuint (manager_count (manager), ==, 0);

  remove_recursive (dir);
  g_print ("ok - a place that is taken is left as it is\n");
}

static void
test_install_needs_resolve (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *index_json =
    pack_index ("Unread Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;

  manager = manager_new (data_dir);

  pack = nuvashalauncher_modpack_new_from_json (index_json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);

  /* The dependencies were never read: the install says so instead of
   * guessing a Minecraft version. */
  instance = install_now (pack, manager, "Unread Pack", NULL, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_LOADER);
  g_clear_error (&error);

  g_assert_cmpuint (manager_count (manager), ==, 0);

  remove_recursive (dir);
  g_print ("ok - an install whose dependencies were never read is refused\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/modpack/install-offline", test_install_offline);
  g_test_add_func ("/modpack/install-rollback", test_install_rolls_back);
  g_test_add_func ("/modpack/install-existing", test_install_leaves_user_data);
  g_test_add_func ("/modpack/install-unresolved", test_install_needs_resolve);
  return g_test_run ();
}
