#include "test-versions-common.h"

static void
test_inheritance_merge (void)
{
  g_autoptr (nuvashalauncherVersionDescription) parent =
    fixture_version ("version-parent.json");
  g_autoptr (nuvashalauncherVersionDescription) child =
    fixture_version ("version-child.json");
  g_autoptr (nuvashalauncherVersionDescription) grandchild =
    fixture_version ("version-grandchild.json");
  g_autoptr (nuvashalauncherVersionDescription) merged = NULL;
  g_autoptr (nuvashalauncherVersionDescription) higher = NULL;
  const nuvashalauncherLibrary *library;
  const nuvashalauncherArgument *argument;

  merged = nuvashalauncher_version_description_merge (parent, child);
  g_assert_nonnull (merged);

  /* The child is the version the user asked for. */
  g_assert_cmpstr (merged->id, ==, "fabric-loader-0.16.9-1.21.1");
  g_assert_cmpstr (merged->main_class, ==,
                   "net.fabricmc.loader.impl.launch.knot.KnotClient");

  /* Everything the child left out comes from the parent. */
  g_assert_null (merged->inherits_from);
  g_assert_cmpstr (merged->assets, ==, "1.21");
  g_assert_nonnull (merged->client);
  g_assert_nonnull (merged->asset_index);
  g_assert_nonnull (merged->logging);
  g_assert_nonnull (merged->java_version);
  g_assert_cmpstr (merged->java_version->component, ==, "java-runtime-delta");
  g_assert_true (merged->modern_arguments);

  /* Libraries and arguments are concatenated, parent first. */
  g_assert_cmpuint (merged->libraries->len, ==, 4);
  library = g_ptr_array_index (merged->libraries, 0);
  g_assert_cmpstr (library->name, ==, "org.lwjgl:lwjgl:3.3.3");
  library = g_ptr_array_index (merged->libraries, 2);
  g_assert_cmpstr (library->name, ==, "net.fabricmc:fabric-loader:0.16.9");

  g_assert_cmpuint (merged->jvm_arguments->len, ==, 5);
  argument = g_ptr_array_index (merged->jvm_arguments, 4);
  g_assert_cmpstr (g_ptr_array_index (argument->values, 0), ==,
                   "-DFabricMcEmu= net.minecraft.client.main.Main ");
  g_assert_cmpuint (merged->game_arguments->len, ==, 4);

  /* Merging copies: neither document it was built from moved. */
  g_assert_cmpuint (parent->libraries->len, ==, 2);
  g_assert_cmpuint (child->libraries->len, ==, 2);
  g_assert_cmpstr (parent->main_class, ==, "net.minecraft.client.main.Main");
  g_assert_cmpstr (parent->id, ==, "1.21.1");

  /* One step further up: the merged profile keeps the chain of the
   * version it inherited from, so the next parent is found. */
  higher = nuvashalauncher_version_description_merge (child, grandchild);
  g_assert_nonnull (higher);
  g_assert_cmpstr (higher->id, ==, "my-profile");
  g_assert_cmpstr (higher->inherits_from, ==, "1.21.1");
  g_assert_cmpstr (higher->main_class, ==,
                   "net.fabricmc.loader.impl.launch.knot.KnotClient");
  g_assert_cmpuint (higher->libraries->len, ==, 3);
  g_assert_cmpuint (higher->jvm_arguments->len, ==, 1);
  g_assert_cmpuint (higher->game_arguments->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (
                     ((nuvashalauncherArgument *) g_ptr_array_index (
                        higher->game_arguments, 0))->values, 0),
                   ==, "--quickPlayPath");

  g_print ("ok - a profile is merged over the version it inherits from\n");
}

static void
test_manifest_fetch (void)
{
  g_autofree char *url = server_url ("/manifest.json");
  g_autofree char *cached = cache_file ("manifest.json");
  g_autofree char *text = NULL;
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);

  call_manifest (&run, &call, url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);
  g_assert_cmpuint (call.manifest->entries->len, ==, 5);
  g_assert_cmpstr (call.manifest->latest_release, ==, "1.21.1");
  g_assert_cmpuint (fixture.hits, ==, 1);

  /* The manifest is kept where the header says it is, and below the
   * cache directory the test made for itself. */
  g_assert_true (g_str_has_prefix (cached, cache_home));
  g_assert_true (g_file_get_contents (cached, &text, NULL, NULL));
  g_assert_nonnull (strstr (text, "\"cycle-a\""));

  /* It is fetched again rather than answered from the copy: a list
   * nobody can check is worse than a visible failure. */
  g_clear_object (&call.manifest);
  g_clear_error (&call.error);

  call_manifest (&run, &call, url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);
  g_assert_cmpuint (fixture.hits, ==, 2);

  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - the manifest is fetched and kept in the cache directory\n");
}

static void
test_version_load (void)
{
  g_autoptr (nuvashalauncherVersionManifest) manifest = local_manifest ();
  g_autofree char *parent_cache = cache_file ("1.21.1.json");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  g_autofree char *text = NULL;
  g_autofree char *sha1 = NULL;

  fixture.hits = 0;

  async_run_init (&run);

  /* The whole line of profiles: profile, loader profile and the
   * version underneath them. */
  call_load (&run, &call, manifest, "my-profile");
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.description);
  g_assert_cmpuint (fixture.hits, ==, 3);

  g_assert_cmpstr (call.description->id, ==, "my-profile");
  g_assert_null (call.description->inherits_from);
  g_assert_cmpstr (call.description->main_class, ==,
                   "net.fabricmc.loader.impl.launch.knot.KnotClient");
  g_assert_cmpstr (call.description->assets, ==, "1.21");
  g_assert_nonnull (call.description->client);

  /* Two documents from the loader, two from the version below it. */
  g_assert_cmpuint (call.description->libraries->len, ==, 5);
  g_assert_cmpuint (call.description->jvm_arguments->len, ==, 5);
  g_assert_cmpuint (call.description->game_arguments->len, ==, 6);

  /* Everything is on disk, under the cache directory of the test. */
  g_assert_true (g_str_has_prefix (parent_cache, cache_home));
  g_assert_true (g_file_get_contents (parent_cache, &text, NULL, NULL));
  sha1 = hash_of (text, strlen (text));
  g_assert_cmpstr (sha1, ==,
                   "cbb2634e1064170a66402097d0735c2a14aab6e7");

  /* Loading the same version again costs no request: the copies on
   * disk answer, having been checked against the manifest. */
  g_clear_object (&call.description);
  g_clear_error (&call.error);

  call_load (&run, &call, manifest, "1.21.1");
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.description);
  g_assert_cmpuint (fixture.hits, ==, 3);
  g_assert_cmpstr (call.description->id, ==, "1.21.1");

  /* A copy that no longer matches is replaced from the verified global
   * file cache, without another network request. */
  g_clear_object (&call.description);
  g_clear_error (&call.error);
  g_assert_true (g_file_set_contents (parent_cache, "not a version", -1,
                                      NULL));

  call_load (&run, &call, manifest, "1.21.1");
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.description);
  g_assert_cmpuint (fixture.hits, ==, 3);

  g_clear_pointer (&text, g_free);
  g_assert_true (g_file_get_contents (parent_cache, &text, NULL, NULL));
  g_clear_pointer (&sha1, g_free);
  sha1 = hash_of (text, strlen (text));
  g_assert_cmpstr (sha1, ==,
                   "cbb2634e1064170a66402097d0735c2a14aab6e7");

  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - a version is downloaded, verified, cached and merged\n");
}

static void
test_version_load_errors (void)
{
  g_autoptr (nuvashalauncherVersionManifest) manifest = local_manifest ();
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);

  /* Nothing is fetched for a version the manifest does not know. */
  call_load (&run, &call, manifest, "not-a-version");
  async_run_wait (&run);

  g_assert_null (call.description);
  g_assert_error (call.error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_NOT_FOUND);
  g_assert_cmpuint (fixture.hits, ==, 0);
  async_call_clear (&call);

  call_load (&run, &call, manifest, "cycle-a");
  async_run_wait (&run);

  g_assert_null (call.description);
  g_assert_error (call.error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_CYCLE);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);

  call_load (&run, &call, manifest, "cycle-a");
  async_run_wait (&run);

  g_assert_null (call.description);
  g_assert_error (call.error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_CYCLE);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);

  async_run_clear (&run);

  g_print ("ok - an unknown version and an inheritance cycle both fail\n");
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  cache_start ();
  fixture_start ();
  test_inheritance_merge ();
  test_manifest_fetch ();
  test_version_load ();
  test_version_load_errors ();
  fixture_stop ();
  cache_stop ();
  return 0;
}
