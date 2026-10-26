#include "test-loaders-common.h"

static void
test_fabric_profile (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionDescription) profile = NULL;
  g_autoptr (nuvashalauncherVersionDescription) vanilla = NULL;
  g_autoptr (nuvashalauncherVersionDescription) merged = NULL;
  const nuvashalauncherLibrary *library = NULL;
  const char *vanilla_document =
    "{\"id\": \"1.20.1\", \"type\": \"release\","
    " \"mainClass\": \"net.minecraft.client.main.Main\","
    " \"assets\": \"12\","
    " \"assetIndex\": {\"id\": \"12\", \"sha1\": \"ab\","
    "                 \"url\": \"https://example.invalid/12.json\"},"
    " \"downloads\": {\"client\": {\"sha1\": \"cd\","
    "                 \"url\": \"https://example.invalid/client.jar\"}},"
    " \"arguments\": {\"jvm\": [\"-cp\", \"${classpath}\"],"
    "               \"game\": [\"--username\", \"${auth_player_name}\"]},"
    " \"libraries\": [{\"name\": \"org.lwjgl:lwjgl:3.3.3\","
    "   \"downloads\": {\"artifact\": {\"path\": \"org/lwjgl/lwjgl/3.3.3/lwjgl.jar\","
    "     \"sha1\": \"ef\", \"url\": \"https://example.invalid/lwjgl.jar\"}}}]}";

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/fabric-profile.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  profile = nuvashalauncher_loaders_profile_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (profile);

  /* The document a loader publishes inherits from a Minecraft version, and
     is what the merged document takes its identity and its main class
     from. */
  g_assert_cmpstr (profile->id, ==, "fabric-loader-0.19.5-1.20.1");
  g_assert_cmpstr (profile->inherits_from, ==, "1.20.1");
  g_assert_cmpstr (profile->main_class, ==,
                   "net.fabricmc.loader.impl.launch.knot.KnotClient");
  g_assert_cmpuint (profile->libraries->len, ==, 4);
  g_assert_cmpuint (profile->jvm_arguments->len, ==, 1);
  {
    const nuvashalauncherArgument *argument = g_ptr_array_index (profile->jvm_arguments, 0);

    g_assert_cmpuint (argument->values->len, ==, 1);
    g_assert_cmpstr (g_ptr_array_index (argument->values, 0), ==,
                     "-DFabricMcEmu= net.minecraft.client.main.Main ");
  }

  /* A library that publishes a maven address instead of a download gets
     the download that address means, because that is what the one resolve
     module knows how to fetch. */
  library = find_library (profile, "org.ow2.asm:asm:9.10.1");
  g_assert_nonnull (library);
  g_assert_nonnull (library->artifact);
  g_assert_cmpstr (library->artifact->path, ==,
                   "org/ow2/asm/asm/9.10.1/asm-9.10.1.jar");
  g_assert_cmpstr (library->artifact->url, ==,
                   "https://maven.fabricmc.net/org/ow2/asm/asm/9.10.1/"
                   "asm-9.10.1.jar");
  g_assert_cmpstr (library->artifact->sha1, ==,
                   "c29635c8a7afa03d74b33c1884df8abb2b3f3dcc");

  /* What the profile says about the bytes is what the resolve can check,
     and a library that publishes none is fetched without a hash. */
  library = find_library (profile, "net.fabricmc:fabric-loader:0.19.5");
  g_assert_nonnull (library);
  g_assert_nonnull (library->artifact);
  g_assert_cmpstr (library->artifact->sha1, ==,
                   "b50ad385829d0000000000000000000000000000");

  library = find_library (profile, "net.fabricmc:sponge-mixin:0.17.4+mixin.0.8.7");
  g_assert_nonnull (library);
  g_assert_nonnull (library->artifact);
  g_assert_cmpstr (library->artifact->url, ==,
                   "https://maven.fabricmc.net/net/fabricmc/sponge-mixin/"
                   "0.17.4+mixin.0.8.7/sponge-mixin-0.17.4+mixin.0.8.7.jar");

  /* The merge a loader instance runs: the loader document on top of the
     Minecraft version it inherits from. */
  vanilla = nuvashalauncher_version_description_new_from_json (vanilla_document, -1,
                                                   &error);
  g_assert_no_error (error);
  g_assert_nonnull (vanilla);

  merged = nuvashalauncher_version_description_merge (vanilla, profile);
  g_assert_nonnull (merged);
  g_assert_cmpstr (merged->id, ==, profile->id);
  g_assert_cmpstr (merged->main_class, ==, profile->main_class);

  /* The Minecraft version keeps what the loader says nothing about: the
     asset index, the client jar and its own libraries. */
  g_assert_cmpstr (merged->assets, ==, "12");
  g_assert_nonnull (merged->asset_index);
  g_assert_nonnull (merged->client);
  g_assert_cmpuint (merged->libraries->len, ==, 5);
  g_assert_nonnull (find_library (merged, "org.lwjgl:lwjgl:3.3.3"));
  g_assert_nonnull (find_library (merged, "org.ow2.asm:asm:9.10.1"));

  /* The libraries of the version come first, the ones of the loader
     after them, and the arguments of both are there. */
  {
    const nuvashalauncherLibrary *first = g_ptr_array_index (merged->libraries, 0);

    g_assert_cmpstr (first->name, ==, "org.lwjgl:lwjgl:3.3.3");
  }
  g_assert_cmpuint (merged->jvm_arguments->len, ==, 3);
  g_assert_cmpuint (merged->game_arguments->len, ==, 2);

  g_print ("ok - a Fabric profile becomes a document that merges over it\n");
}

static void
test_quilt_profile (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionDescription) profile = NULL;
  const nuvashalauncherLibrary *library = NULL;

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/quilt-profile.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  profile = nuvashalauncher_loaders_profile_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (profile);

  g_assert_cmpstr (profile->id, ==, "quilt-loader-0.30.1-26.3");
  g_assert_cmpstr (profile->inherits_from, ==, "26.3");
  g_assert_cmpstr (profile->main_class, ==,
                   "org.quiltmc.loader.impl.launch.knot.KnotClient");
  g_assert_cmpuint (profile->libraries->len, ==, 4);

  /* One jvm argument and one that only macOS gets, like a document of
     Mojang's own. */
  g_assert_cmpuint (profile->jvm_arguments->len, ==, 2);

  library = find_library (profile, "org.quiltmc:quilt-json5:1.0.4+final");
  g_assert_nonnull (library);
  g_assert_nonnull (library->artifact);
  g_assert_cmpstr (library->artifact->url, ==,
                   "https://maven.quiltmc.org/repository/release/org/quiltmc/"
                   "quilt-json5/1.0.4+final/quilt-json5-1.0.4+final.jar");

  /* A library that publishes no hash is fetched without one: there is
     nothing to verify, which the resolve module allows. */
  g_assert_null (library->artifact->sha1);

  library = find_library (profile, "org.ow2.asm:asm:9.9");
  g_assert_nonnull (library);
  g_assert_cmpstr (library->artifact->sha1, ==,
                   "c29635c8a7afa03d74b33c1884df8abb2b3f3dcc");

  g_print ("ok - a Quilt profile is a document, with or without hashes\n");
}

static void
test_profile_without_a_maven_address (void)
{
  g_autoptr (GError) error = NULL;

  /* A library that names no jar at all is refused: a document that
     silently loses a library would start a game that cannot work. */
  g_assert_null (nuvashalauncher_loaders_profile_from_json (
    "{\"id\": \"x\", \"type\": \"release\","
    " \"libraries\": [{\"name\": \"no:where:1.0\"}]}", -1, &error));
  g_assert_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_MAVEN);
  g_clear_error (&error);

  /* Coordinates that make no path are refused as well, because the path
     they would write into the library directory has to be a name. */
  g_assert_null (nuvashalauncher_loaders_profile_from_json (
    "{\"id\": \"x\", \"type\": \"release\","
    " \"libraries\": [{\"name\": \"../evil:evil:1.0\","
    "                 \"url\": \"https://example.invalid/\"}]}", -1, &error));
  g_assert_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_MAVEN);

  g_print ("ok - a profile with no usable maven address is refused\n");
}

static void
test_install_of_a_loader_that_publishes_none (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionDescription) document = NULL;
  Wait wait = { 0 };

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/fabric-profile.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  document = nuvashalauncher_loaders_profile_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (document);

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.watchdog = g_timeout_add_seconds (20, watchdog_fired, &wait);

  wait.operation = nuvashalauncher_loaders_install_async (nuvashalauncher_LOADER_FABRIC, "1.20.1",
                                              "0.19.5", document, "/bin/true",
                                              NULL, NULL, NULL,
                                              on_install_finished, &wait);

  g_main_loop_run (wait.loop);
  g_source_remove (wait.watchdog);
  g_main_loop_unref (wait.loop);

  g_assert_null (wait.install);
  g_assert_error (wait.error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_UNSUPPORTED);
  g_assert_nonnull (strstr (wait.error->message, "Fabric"));
  g_clear_error (&wait.error);

  g_print ("ok - a loader with no installer is refused for an install\n");
}

static void
test_profile_of_a_loader_that_publishes_an_installer (void)
{
  Wait wait = { 0 };

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.watchdog = g_timeout_add_seconds (20, watchdog_fired, &wait);

  wait.operation = nuvashalauncher_loaders_profile_async (nuvashalauncher_LOADER_FORGE, "1.20.1",
                                              "1.20.1-47.2.0",
                                              on_profile_finished, &wait);

  g_main_loop_run (wait.loop);
  g_source_remove (wait.watchdog);
  g_main_loop_unref (wait.loop);

  g_assert_null (wait.profile);
  g_assert_error (wait.error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_UNSUPPORTED);
  g_assert_nonnull (strstr (wait.error->message, "nuvashalauncher_loaders_install_async"));
  g_clear_error (&wait.error);

  g_print ("ok - a loader with no profile is refused for one\n");
}

static void
test_cancelled_versions_deliver_nothing (void)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  Wait wait = { 0 };

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.watchdog = g_timeout_add_seconds (20, watchdog_fired, &wait);

  /* Cancelled before it starts, so nothing is asked of the network: the
     request is what the last screen left behind. */
  g_cancellable_cancel (cancellable);

  wait.operation = nuvashalauncher_loaders_versions_async (nuvashalauncher_LOADER_FABRIC, "1.20.1",
                                               on_versions_finished, &wait);
  nuvashalauncher_task_scope_follow_cancellable (wait.operation, cancellable);

  g_main_loop_run (wait.loop);
  g_source_remove (wait.watchdog);
  g_main_loop_unref (wait.loop);

  g_assert_null (wait.versions);
  g_assert_nonnull (wait.error);
  g_assert_true (
    g_error_matches (wait.error, nuvashalauncher_DOWNLOAD_ERROR,
                     nuvashalauncher_DOWNLOAD_ERROR_CANCELLED) ||
    g_error_matches (wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
  g_clear_error (&wait.error);

  g_print ("ok - a cancelled version load hands nothing to a screen\n");
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  test_fabric_profile ();
  test_quilt_profile ();
  test_profile_without_a_maven_address ();
  test_install_of_a_loader_that_publishes_none ();
  test_profile_of_a_loader_that_publishes_an_installer ();
  test_cancelled_versions_deliver_nothing ();
  return EXIT_SUCCESS;
}
