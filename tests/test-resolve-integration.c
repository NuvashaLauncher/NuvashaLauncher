#include "test-resolve-common.h"

static void
test_resolve_modern (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *natives_dir = g_build_filename (dir, "instance", NULL);
  g_autofree char *assets_url = g_strdup (fixture.base);
  g_autofree char *client_jar = NULL;
  g_autofree char *library_jar = NULL;
  g_autofree char *natives_jar = NULL;
  g_autofree char *native_file = NULL;
  g_autofree char *logging_file = NULL;
  g_autofree char *object = NULL;
  g_autofree char *path = NULL;
  g_autofree char *text = NULL;
  g_autofree char *classpath = NULL;
  AsyncRun run = { 0 };
  ResolveCall call = { 0 };
  Served served;

  g_autofree char *manifest_url = server_url ("/manifest.json");
  fill_served (&served, "test-version", "test");
  serve_version (&served, dir);

  async_run_init (&run);
  fetch_manifest (&run, &call, manifest_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);
  g_assert_cmpuint (fixture.hits, ==, 1);

  start_resolve (&run, &call, call.manifest, "test-version", data_dir,
                 natives_dir, assets_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.resolved);

  /* Everything the document named is on the disk where it belongs. */
  client_jar = g_build_filename (data_dir, "versions", "test-version",
                                 "test-version.jar", NULL);
  library_jar = g_build_filename (data_dir, "libraries", "org", "lwjgl",
                                  "lwjgl", "3.3.3", "lwjgl-3.3.3.jar", NULL);
  natives_jar = g_build_filename (data_dir, "libraries", "org", "lwjgl",
                                  "lwjgl", "3.3.3",
                                  "lwjgl-3.3.3-natives-linux.jar", NULL);
  /* The logging file keeps the name the document gave it. */
  logging_file = g_build_filename (data_dir, "logging", "client.xml", NULL);

  g_free (text);
  text = read_text_file (client_jar);
  g_assert_cmpstr (text, ==, served.client->str);
  g_clear_pointer (&text, g_free);

  g_free (text);
  text = read_text_file (library_jar);
  g_assert_cmpstr (text, ==, served.library->str);
  g_clear_pointer (&text, g_free);

  g_assert_true (g_file_test (natives_jar, G_FILE_TEST_EXISTS));

  /* The library of another system was never asked for. */
  g_free (path);
  path = g_build_filename (data_dir, "libraries", "com", "example", "mac-only",
                           "1.0", "mac-only.jar", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* The natives jar was unpacked, without what it excludes. */
  native_file = g_build_filename (natives_dir, "liblwjgl.so", NULL);
  g_free (text);
  text = read_text_file (native_file);
  g_assert_cmpstr (text, ==, "the lwjgl native\n");
  g_clear_pointer (&text, g_free);

  g_free (path);
  path = g_build_filename (natives_dir, "META-INF", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* The asset objects are named after their own hash. */
  for (guint i = 0; i < G_N_ELEMENTS (served.objects); i++)
    {
      g_autofree char *bucket = g_strndup (served.hashes[i], 2);

      g_free (object);
      object = g_build_filename (data_dir, "assets", "objects", bucket,
                                 served.hashes[i], NULL);
      text = read_text_file (object);
      g_assert_cmpstr (text, ==, served.objects[i]->str);
      g_clear_pointer (&text, g_free);
    }

  /* And the answer says so. */
  g_assert_cmpstr (call.resolved->version_id, ==, "test-version");
  g_assert_cmpstr (call.resolved->version_type, ==, "release");
  g_assert_cmpstr (call.resolved->main_class, ==,
                   "net.minecraft.client.main.Main");
  g_assert_cmpstr (call.resolved->client_jar, ==, client_jar);
  g_assert_cmpstr (call.resolved->natives_directory, ==, natives_dir);
  g_free (path);
  path = g_build_filename (data_dir, "assets", NULL);
  g_assert_cmpstr (call.resolved->assets_directory, ==, path);
  g_assert_cmpstr (call.resolved->assets_index_name, ==, "test");
  g_assert_cmpint (call.resolved->java_major_version, ==, 21);

  classpath = g_strdup_printf ("%s:%s", library_jar, client_jar);
  g_assert_cmpstr (call.resolved->classpath, ==, classpath);

  g_free (path);
  path = g_strdup_printf ("-Dlog4j.configurationFile=%s", logging_file);
  g_assert_true (g_file_test (logging_file, G_FILE_TEST_EXISTS));
  g_assert_cmpstr (call.resolved->logging_argument, ==, path);

  /* The placeholders nuvashalauncher knows are in, the ones it does not are not. */
  g_assert_cmpuint (call.resolved->jvm_arguments->len, ==, 3);
  g_free (path);
  path = g_strdup_printf ("-Djava.library.path=%s", natives_dir);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->jvm_arguments, 0), ==, path);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->jvm_arguments, 1), ==,
                   "-cp");
  g_assert_cmpstr (g_ptr_array_index (call.resolved->jvm_arguments, 2), ==,
                   classpath);

  g_assert_cmpuint (call.resolved->game_arguments->len, ==, 6);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 1), ==,
                   "${auth_player_name}");
  g_free (path);
  path = g_build_filename (data_dir, "assets", NULL);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 3), ==,
                   path);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 5), ==,
                   "test");

  /* Progress was reported and ended with the last file of the last
     stage. */
  g_assert_cmpuint (call.progress_calls, >, 0);
  g_assert_cmpstr (call.last_stage, ==, "assets");
  g_assert_cmpuint (call.last_done, ==, call.last_total);

  /* A second resolve asks the network for nothing. */
  g_clear_pointer (&call.resolved, nuvashalauncher_resolved_version_free);
  g_clear_error (&call.error);
  fixture.hits = 0;

  start_resolve (&run, &call, call.manifest, "test-version", data_dir,
                 natives_dir, assets_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.resolved);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_cmpstr (call.resolved->classpath, ==, classpath);

  g_clear_object (&call.manifest);
  g_clear_pointer (&call.resolved, nuvashalauncher_resolved_version_free);
  g_clear_error (&call.error);
  g_clear_pointer (&text, g_free);
  g_free (call.last_stage);
  free_served (&served);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a version is resolved, then served from the caches\n");
}

static void
test_resolve_legacy_assets (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *natives_dir = g_build_filename (dir, "instance", NULL);
  g_autofree char *assets_url = g_strdup (fixture.base);
  g_autofree char *copy = NULL;
  g_autofree char *path = NULL;
  g_autofree char *text = NULL;
  AsyncRun run = { 0 };
  ResolveCall call = { 0 };
  Served served;

  /* A version with an old index: virtual, and mapping its paths onto the
     resources of the game. */
  g_autofree char *manifest_url = server_url ("/manifest.json");
  fill_served (&served, "test-legacy", "legacy");
  served.virtual_index = TRUE;
  served.paths[0] = (char *) "sounds/step.ogg";
  served.paths[1] = NULL;
  serve_version (&served, dir);

  async_run_init (&run);
  fetch_manifest (&run, &call, manifest_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);

  start_resolve (&run, &call, call.manifest, "test-legacy", data_dir,
                 natives_dir, assets_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.resolved);

  /* The game is pointed at the virtual directory of the index and told
     no index name. */
  g_free (path);
  path = g_build_filename (data_dir, "assets", "virtual", "legacy", NULL);
  g_assert_cmpstr (call.resolved->assets_directory, ==, path);
  g_assert_null (call.resolved->assets_index_name);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 5), ==,
                   "legacy");

  /* Every object the index gives a path to was copied under it, with the
     prefix the game looks under. */
  copy = g_build_filename (data_dir, "assets", "virtual", "legacy",
                           "minecraft", "sounds", "step.ogg", NULL);
  g_free (text);
  text = read_text_file (copy);
  g_assert_cmpstr (text, ==, served.objects[0]->str);

  /* The object without a path was fetched but not copied. */
  {
    g_autofree char *bucket = g_strndup (served.hashes[1], 2);
    g_autofree char *object = g_build_filename (
        data_dir, "assets", "objects", bucket, served.hashes[1], NULL);

    g_assert_true (g_file_test (object, G_FILE_TEST_EXISTS));
  }

  g_clear_object (&call.manifest);
  g_clear_pointer (&call.resolved, nuvashalauncher_resolved_version_free);
  g_clear_error (&call.error);
  g_clear_pointer (&text, g_free);
  g_free (call.last_stage);
  free_served (&served);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a virtual asset index is copied where the game reads it\n");
}

static void
test_resolve_failures (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *assets_url = g_strdup (fixture.base);
  AsyncRun run = { 0 };
  ResolveCall call = { 0 };
  Served served;

  g_autofree char *manifest_url = server_url ("/manifest.json");
  fill_served (&served, "test-version", "test");
  serve_version (&served, dir);

  async_run_init (&run);
  fetch_manifest (&run, &call, manifest_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);

  /* A version the manifest does not know is not resolved. */
  start_resolve (&run, &call, call.manifest, "no-such-version", data_dir, NULL,
                 assets_url);
  async_run_wait (&run);

  g_assert_null (call.resolved);
  g_assert_error (call.error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_NOT_FOUND);

  g_clear_error (&call.error);
  g_clear_object (&call.manifest);
  g_free (call.last_stage);
  free_served (&served);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a version that cannot be resolved fails as it should\n");
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  cache_start ();
  fixture_start ();
  test_resolve_modern ();
  test_resolve_legacy_assets ();
  test_resolve_failures ();
  fixture_stop ();
  cache_stop ();
  return EXIT_SUCCESS;
}
