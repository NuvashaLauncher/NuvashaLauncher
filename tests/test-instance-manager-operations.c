#include "test-instance-manager-common.h"

/* Helpers for the asynchronous manager operations tested in this file. */
static gboolean
async_timeout (gpointer user_data)
{
  AsyncCall *call = user_data;
  call->timed_out = TRUE;
  g_main_loop_quit (call->loop);
  return G_SOURCE_REMOVE;
}

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
  call->result = nuvashalauncher_instance_manager_duplicate_finish (
    nuvashalauncher_INSTANCE_MANAGER (source), result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
delete_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;
  call->success = nuvashalauncher_instance_manager_delete_finish (
    nuvashalauncher_INSTANCE_MANAGER (source), result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;
  g_main_loop_quit (call->loop);
}

static void
open_folder_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;
  call->success = nuvashalauncher_instance_manager_open_folder_finish (
    nuvashalauncher_INSTANCE_MANAGER (source), result, &call->error);
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

static void
test_rename (void)
{
  g_autofree char *data_dir = make_tmp_dir ();
  g_autoptr (GError) error = NULL;

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Original", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_assert_true (nuvashalauncher_instance_manager_rename (manager, instance,
                                              "  Renamed Pack  ", &error));
  g_assert_no_error (error);

  g_autofree char *name = prop_string (instance, "name");
  g_assert_cmpstr (name, ==, "Renamed Pack");

  /* The new name is on disk. */
  g_autoptr (nuvashalauncherInstanceManager) fresh = nuvashalauncher_instance_manager_new (data_dir);
  g_assert_true (nuvashalauncher_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 1);

  g_autoptr (nuvashalauncherInstance) loaded =
    g_list_model_get_item (
      G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (fresh)), 0);
  g_autofree char *loaded_name = prop_string (loaded, "name");
  g_assert_cmpstr (loaded_name, ==, "Renamed Pack");

  /* A blank name is refused and the old name survives. */
  g_assert_false (nuvashalauncher_instance_manager_rename (manager, instance, "   ",
                                               &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_free (name);
  name = prop_string (instance, "name");
  g_assert_cmpstr (name, ==, "Renamed Pack");

  /* A path separator is reported specifically and leaves the old name. */
  g_assert_false (nuvashalauncher_instance_manager_rename (manager, instance, "/",
                                               &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_assert_cmpstr (error->message, ==,
                   "Instances cannot contain / in their name");
  g_clear_error (&error);

  /* Renaming an instance the manager does not know fails. */
  g_autoptr (nuvashalauncherInstance) stranger =
    nuvashalauncher_instance_new (NULL, "Stranger", "/tmp/nuvashalauncher-test-stranger", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_no_error (error);
  g_assert_nonnull (stranger);

  g_assert_false (nuvashalauncher_instance_manager_rename (manager, stranger,
                                               "Renamed", &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_NOT_FOUND);
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
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_manager_create (manager, "Model", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);

  g_assert_no_error (error);
  g_assert_true (nuvashalauncher_instance_manager_set_string (
    manager, instance, "jvm-args", "-Xmx2G", &error));
  g_assert_no_error (error);
  g_assert_true (nuvashalauncher_instance_manager_set_loader (
    manager, instance, nuvashalauncher_LOADER_FABRIC, "0.15.0", &error));
  g_assert_no_error (error);
  g_assert_false (nuvashalauncher_instance_manager_set_string (
    manager, instance, "not-a-property", "value", &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  g_autoptr (nuvashalauncherInstanceManager) fresh = nuvashalauncher_instance_manager_new (data_dir);
  g_assert_true (nuvashalauncher_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_autoptr (nuvashalauncherInstance) loaded = g_list_model_get_item (
    G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (fresh)), 0);
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Original", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_FABRIC, "0.16.9", &error);
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

   call.operation = nuvashalauncher_instance_manager_duplicate_async (
     manager, instance, duplicate_finished, &call);
  async_call_run (&call);
  g_assert_no_error (call.error);
  g_assert_nonnull (call.result);

  nuvashalauncherInstance *copy = call.result;
  g_assert_cmpstr (nuvashalauncher_instance_get_id (copy), !=,
                   nuvashalauncher_instance_get_id (instance));
  g_assert_cmpstr (nuvashalauncher_instance_get_id (copy), !=, "");
  g_assert_true (g_uuid_string_is_valid (nuvashalauncher_instance_get_id (copy)));

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

  g_autoptr (nuvashalauncherInstanceManager) fresh = nuvashalauncher_instance_manager_new (data_dir);
  g_assert_true (nuvashalauncher_instance_manager_load (fresh, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (fresh), ==, 2);
  g_assert_nonnull (nuvashalauncher_instance_manager_get_by_id (
    fresh, nuvashalauncher_instance_get_id (instance)));
  g_assert_nonnull (nuvashalauncher_instance_manager_get_by_id (
    fresh, nuvashalauncher_instance_get_id (copy)));

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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Vanishing", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_autofree char *instances_root =
    g_build_filename (data_dir, "instances", NULL);
  g_autofree char *icon = g_build_filename (directory, "icon.png", NULL);

  g_assert_cmpuint (count_entries (instances_root), ==, 1);
  /* New instances receive the default icon, so remove that generated file
   * before simulating an externally deleted instance directory. */
  g_assert_true (nuvashalauncher_fs_unlink (icon, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
  g_assert_cmpuint (count_entries (instances_root), ==, 0);

  /* Copying a directory that is gone fails with an error, creates
   * nothing and keeps the list untouched. */
  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvashalauncher_instance_manager_duplicate_async (
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Kept", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvashalauncher_instance_manager_delete_async (
     manager, instance, FALSE, delete_finished, &call);
  async_call_run (&call);
  g_assert_no_error (call.error);
  g_assert_true (call.success);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

  g_autoptr (nuvashalauncherInstanceManager) fresh = nuvashalauncher_instance_manager_new (data_dir);
  g_assert_true (nuvashalauncher_instance_manager_load (fresh, &error));
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Gone", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_autofree char *level_path =
    g_build_filename (directory, "saves", "level.dat", NULL);
  write_file (level_path, "data");
  g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvashalauncher_instance_manager_delete_async (
     manager, instance, TRUE, delete_finished, &call);
  async_call_run (&call);
  g_assert_no_error (call.error);
  g_assert_true (call.success);

  g_assert_cmpuint (manager_count (manager), ==, 0);
  g_assert_false (g_file_test (directory, G_FILE_TEST_EXISTS));

  g_autoptr (nuvashalauncherInstanceManager) fresh = nuvashalauncher_instance_manager_new (data_dir);
  g_assert_true (nuvashalauncher_instance_manager_load (fresh, &error));
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  g_autoptr (nuvashalauncherInstance) stranger =
    nuvashalauncher_instance_new (NULL, "Stranger", "/tmp/nuvashalauncher-test-stranger", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_no_error (error);
  g_assert_nonnull (stranger);

  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

   call.operation = nuvashalauncher_instance_manager_delete_async (
     manager, stranger, FALSE, delete_finished, &call);
  async_call_run (&call);
  g_assert_error (call.error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_NOT_FOUND);
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Folderless", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *directory = prop_string (instance, "directory");
  g_autofree char *icon = g_build_filename (directory, "icon.png", NULL);
  g_assert_true (nuvashalauncher_fs_unlink (icon, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_rmdir (directory), ==, 0);

  /* The directory is gone, so nothing is launched: the test fails
   * with an error instead of needing a file manager. */
  AsyncCall call = { 0 };
  call.loop = g_main_loop_new (NULL, FALSE);

  call.operation =
    nuvashalauncher_instance_manager_open_folder_async (manager, instance,
                                            open_folder_finished, &call);
  async_call_run (&call);
  g_assert_error (call.error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *first =
    nuvashalauncher_instance_manager_create (manager, "One", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);

  nuvashalauncherInstance *second =
    nuvashalauncher_instance_manager_create (manager, "Two", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
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
  g_assert_true (nuvashalauncher_instance_manager_check_new_directory (
    manager, first, elsewhere, &canonical, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (canonical, ==, elsewhere);

  /* A relative path is not. */
  g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
    manager, first, "relative/path", NULL, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  {
    g_autofree char *escape =
      g_build_filename (data_dir, "elsewhere", "..", "climb", NULL);

    g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
      manager, first, escape, NULL, &error));
    g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                    nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);
  }

  /* The filesystem root is not. */
  g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
    manager, first, G_DIR_SEPARATOR_S, NULL, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* The directory the instance already has is not a change. */
  g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
    manager, first, first_directory, NULL, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  /* The directory of another instance is not, neither is one inside it
     or one that would contain it. */
  g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
    manager, first, second_directory, NULL, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                  nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
  g_clear_error (&error);

  {
    g_autofree char *inside =
      g_build_filename (second_directory, "nested", NULL);

    g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
      manager, first, inside, NULL, &error));
    g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                    nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);
  }

  {
    g_autofree char *parent = g_path_get_dirname (second_directory);

    g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
      manager, first, parent, NULL, &error));
    g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                    nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
    g_clear_error (&error);
  }

  /* A path that exists already is not taken over. */
  {
    g_autofree char *taken =
      g_build_filename (elsewhere_parent, "taken", NULL);

    write_file (taken, "not an instance");

    g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
      manager, first, taken, NULL, &error));
    g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                    nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
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

    g_assert_false (nuvashalauncher_instance_manager_check_new_directory (
      manager, first, orphan, NULL, &error));
    g_assert_error (error, nuvashalauncher_INSTANCE_MANAGER_ERROR,
                    nuvashalauncher_INSTANCE_MANAGER_ERROR_INVALID_PATH);
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Mover", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
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

   call.operation = nuvashalauncher_instance_manager_change_directory_async (
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
    g_autoptr (nuvashalauncherInstanceManager) reloaded =
      nuvashalauncher_instance_manager_new (data_dir);

    g_assert_true (nuvashalauncher_instance_manager_load (reloaded, &error));
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

  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (data_dir);

  nuvashalauncherInstance *instance =
    nuvashalauncher_instance_manager_create (manager, "Keeper", NULL, "1.21.1",
                                 nuvashalauncher_LOADER_VANILLA, NULL, &error);
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

   call.operation = nuvashalauncher_instance_manager_change_directory_async (
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
  (void) argc; (void) argv;
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
  return 0;
}
