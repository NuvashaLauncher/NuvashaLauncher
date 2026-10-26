#include "test-versions-common.h"

static void
test_manifest_parsing (void)
{
  g_autofree char *text = fixture_text ("manifest.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionManifest) manifest = NULL;
  nuvashalauncherMcVersionType type = nuvashalauncher_MC_TYPE_RELEASE;
  const nuvashalauncherVersionEntry *entry;

  manifest = nuvashalauncher_version_manifest_new_from_json (text, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (manifest);

  g_assert_cmpstr (manifest->latest_release, ==, "1.21.1");
  g_assert_cmpstr (manifest->latest_snapshot, ==, "24w33one");
  g_assert_cmpuint (manifest->entries->len, ==, 5);

  entry = g_ptr_array_index (manifest->entries, 0);
  g_assert_cmpstr (entry->id, ==, "24w33one");
  g_assert_cmpint (entry->type, ==, nuvashalauncher_MC_TYPE_SNAPSHOT);
  g_assert_cmpstr (entry->sha1, ==,
                   "655c2da7d6815e84864a0f9a055c90e36337b0b4");
  g_assert_cmpstr (entry->release_time, ==, "2024-08-15T10:00:00+00:00");
  g_assert_true (g_str_has_prefix (entry->url,
                                   "https://piston-meta.mojang.com/"));

  entry = nuvashalauncher_version_manifest_find (manifest, "b1.7.3");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->type, ==, nuvashalauncher_MC_TYPE_OLD_BETA);

  entry = nuvashalauncher_version_manifest_find (manifest, "rd-132211");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->type, ==, nuvashalauncher_MC_TYPE_OLD_ALPHA);

  g_assert_null (nuvashalauncher_version_manifest_find (manifest, "not-a-version"));

  /* The type strings are the four Mojang writes, no others. */
  g_assert_true (nuvashalauncher_mc_version_type_from_string ("old_beta", &type));
  g_assert_cmpint (type, ==, nuvashalauncher_MC_TYPE_OLD_BETA);
  g_assert_false (nuvashalauncher_mc_version_type_from_string ("beta", &type));
  g_assert_false (nuvashalauncher_mc_version_type_from_string (NULL, &type));
  g_assert_cmpstr (nuvashalauncher_mc_version_type_to_string (nuvashalauncher_MC_TYPE_RELEASE), ==,
                   "release");
  g_assert_null (nuvashalauncher_mc_version_type_to_string ((nuvashalauncherMcVersionType) 99));

  g_print ("ok - the manifest lists releases, snapshots and the old versions\n");
}

static void
test_manifest_malformed (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionManifest) manifest = NULL;
  g_autofree char *incomplete = fixture_text ("manifest-incomplete.json");

  /* Not JSON at all. */
  manifest = nuvashalauncher_version_manifest_new_from_json ("{\"latest\":", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* JSON, but not the shape of a manifest. */
  manifest = nuvashalauncher_version_manifest_new_from_json ("[]", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* Nothing says which version is the latest. */
  manifest = nuvashalauncher_version_manifest_new_from_json ("{\"versions\": []}", -1,
                                                 &error);
  g_assert_null (manifest);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_MISSING);
  g_clear_error (&error);

  /* A latest without its snapshot. */
  manifest = nuvashalauncher_version_manifest_new_from_json (
    "{\"latest\": {\"release\": \"1.21.1\"}, \"versions\": []}", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_MISSING);
  g_clear_error (&error);

  /* An entry without a sha1 cannot be verified, so it is not an
   * entry. */
  manifest = nuvashalauncher_version_manifest_new_from_json (incomplete, -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_MISSING);
  g_clear_error (&error);

  /* A type outside the four. */
  manifest = nuvashalauncher_version_manifest_new_from_json (
    "{\"latest\": {\"release\": \"x\", \"snapshot\": \"y\"},"
    " \"versions\": [{\"id\": \"x\", \"type\": \"beta\","
    " \"url\": \"https://example.invalid/x.json\","
    " \"sha1\": \"1111111111111111111111111111111111111111\","
    " \"releaseTime\": \"2024-01-01T00:00:00+00:00\"}]}", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  g_print ("ok - a manifest that is not a manifest is rejected\n");
}

static void
test_modern_version (void)
{
  g_autoptr (nuvashalauncherVersionDescription) version =
    fixture_version ("version-modern.json");
  const nuvashalauncherDownloadInfo *asset_index;
  const nuvashalauncherArgument *argument;
  const nuvashalauncherLibrary *library;

  g_assert_cmpstr (version->id, ==, "1.21.1");
  g_assert_cmpint (version->type, ==, nuvashalauncher_MC_TYPE_RELEASE);
  g_assert_cmpstr (version->main_class, ==, "net.minecraft.client.main.Main");
  g_assert_cmpstr (version->assets, ==, "1.21");
  g_assert_null (version->inherits_from);

  asset_index = version->asset_index;
  g_assert_nonnull (asset_index);
  g_assert_cmpstr (asset_index->id, ==, "1.21");
  g_assert_cmpstr (asset_index->sha1, ==,
                   "abfaa525f923f807df8b4e4d29c1b5e3a104adbe");
  g_assert_cmpint (asset_index->size, ==, 597035);
  g_assert_cmpint (asset_index->total_size, ==, 483782790);
  g_assert_true (g_str_has_prefix (asset_index->url,
                                   "https://piston-meta.mojang.com/"));

  g_assert_nonnull (version->client);
  g_assert_cmpstr (version->client->sha1, ==,
                   "e877b6a07acd633fb3bb475002175cec036e7b87");
  g_assert_cmpint (version->client->size, ==, 41483720);

  g_assert_nonnull (version->java_version);
  g_assert_cmpstr (version->java_version->component, ==,
                   "java-runtime-delta");
  g_assert_cmpint (version->java_version->major_version, ==, 21);

  g_assert_nonnull (version->logging);
  g_assert_cmpstr (version->logging->argument, ==,
                   "-Dlog4j.configurationFile=${path}");
  g_assert_cmpstr (version->logging->type, ==, "log4j2-xml");
  g_assert_nonnull (version->logging->file);
  g_assert_cmpstr (version->logging->file->id, ==, "client-1.21.xml");

  g_assert_true (version->modern_arguments);
  g_assert_null (version->minecraft_arguments);

  /* The default heap sizes Mojang writes in a group of their own are
   * not arguments of the game and are left alone. */
  g_assert_cmpuint (version->jvm_arguments->len, ==, 8);
  g_assert_cmpuint (version->game_arguments->len, ==, 11);

  argument = g_ptr_array_index (version->jvm_arguments, 0);
  g_assert_null (argument->rules);
  g_assert_cmpuint (argument->values->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (argument->values, 0), ==,
                   "-Djava.library.path=${natives_directory}");

  argument = g_ptr_array_index (version->jvm_arguments, 3);
  g_assert_nonnull (argument->rules);
  g_assert_cmpuint (argument->rules->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (argument->values, 0), ==,
                   "-XstartOnFirstThread");

  /* An argument may stand for several words at once. */
  argument = g_ptr_array_index (version->game_arguments, 9);
  g_assert_nonnull (argument->rules);
  g_assert_cmpuint (argument->values->len, ==, 4);
  g_assert_cmpstr (g_ptr_array_index (argument->values, 0), ==, "--width");
  g_assert_cmpstr (g_ptr_array_index (argument->values, 3), ==, "720");

  g_assert_cmpuint (version->libraries->len, ==, 4);

  library = g_ptr_array_index (version->libraries, 0);
  g_assert_cmpstr (library->name, ==, "org.lwjgl:lwjgl:3.3.3");
  g_assert_nonnull (library->artifact);
  g_assert_cmpstr (library->artifact->path, ==,
                   "org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar");
  g_assert_cmpstr (library->artifact->sha1, ==,
                   "554836d32f2391b4c38a008189834c16de15d123");
  g_assert_cmpint (library->artifact->size, ==, 729357);
  g_assert_null (library->rules);
  g_assert_null (library->natives);

  library = g_ptr_array_index (version->libraries, 1);
  g_assert_nonnull (library->rules);
  g_assert_cmpstr (library->name, ==, "com.mojang:jtracy:1.14.38:natives-linux");

  /* A document without the legacy line answers with no words at all. */
  {
    g_autoptr (GPtrArray) argv = nuvashalauncher_version_description_legacy_argv (version);

    g_assert_cmpuint (argv->len, ==, 0);
  }

  g_print ("ok - a modern version document keeps its arguments and downloads\n");
}

static void
test_legacy_version (void)
{
  g_autoptr (nuvashalauncherVersionDescription) version =
    fixture_version ("version-legacy.json");
  const nuvashalauncherLibrary *library;
  g_autoptr (GPtrArray) argv = NULL;

  g_assert_cmpstr (version->id, ==, "1.5.2");
  g_assert_cmpstr (version->main_class, ==,
                   "net.minecraft.launchwrapper.Launch");
  g_assert_false (version->modern_arguments);
  g_assert_cmpuint (version->jvm_arguments->len, ==, 0);
  g_assert_cmpuint (version->game_arguments->len, ==, 0);
  g_assert_nonnull (version->minecraft_arguments);

  argv = nuvashalauncher_version_description_legacy_argv (version);
  g_assert_cmpuint (argv->len, ==, 12);
  g_assert_cmpstr (g_ptr_array_index (argv, 0), ==, "--username");
  g_assert_cmpstr (g_ptr_array_index (argv, 11), ==,
                   "net.fabricmc.loader.launch.knot.KnotClient");

  /* A version this old has no log4j configuration of its own. */
  g_assert_null (version->logging);
  g_assert_nonnull (version->java_version);
  g_assert_cmpint (version->java_version->major_version, ==, 8);
  g_assert_cmpstr (version->asset_index->id, ==, "legacy");

  g_assert_cmpuint (version->libraries->len, ==, 4);

  /* The natives jar of this machine, and the extraction rule that
   * goes with it. */
  library = g_ptr_array_index (version->libraries, 0);
  g_assert_nonnull (library->natives);
  g_assert_nonnull (library->classifiers);
  g_assert_cmpstr (g_hash_table_lookup (library->natives, "linux"), ==,
                   "natives-linux");
  g_assert_cmpstr (g_hash_table_lookup (library->natives, "osx"), ==,
                   "natives-osx");

  {
    const nuvashalauncherDownloadInfo *info =
      g_hash_table_lookup (library->classifiers, "natives-linux");

    g_assert_cmpstr (info->path, ==,
                     "org/lwjgl/lwjgl/lwjgl-platform/2.9.0/"
                     "lwjgl-platform-2.9.0-natives-linux.jar");
    g_assert_cmpint (info->size, ==, 569061);
    g_assert_true (g_str_has_prefix (info->url,
                                     "https://libraries.minecraft.net/"));
  }

  g_assert_nonnull (library->exclude);
  g_assert_cmpuint (library->exclude->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (library->exclude, 0), ==, "META-INF/");

  /* A library with no downloads at all still parses. */
  library = g_ptr_array_index (version->libraries, 3);
  g_assert_cmpstr (library->name, ==, "com.example:linux-only:1.0");
  g_assert_null (library->artifact);
  g_assert_nonnull (library->rules);

  g_print ("ok - a legacy version document keeps minecraftArguments and natives\n");
}

static void
test_missing_fields (void)
{
  g_autofree char *incomplete = fixture_text ("version-incomplete.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionDescription) version = NULL;

  /* The identity of a document is what it must carry. */
  version = nuvashalauncher_version_description_new_from_json (incomplete, -1, &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_MISSING);
  g_clear_error (&error);

  version = nuvashalauncher_version_description_new_from_json ("{\"id\": \"x\"}", -1,
                                                   &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_MISSING);
  g_clear_error (&error);

  g_print ("ok - a version without id or type is rejected\n");
}

static void
test_malformed_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionDescription) version = NULL;

  /* Not JSON. */
  version = nuvashalauncher_version_description_new_from_json ("<html>", -1, &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* JSON that is not an object. */
  version = nuvashalauncher_version_description_new_from_json ("\"1.21.1\"", -1, &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* A type outside the four. */
  version = nuvashalauncher_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"beta\"}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* A field of the wrong shape. */
  version = nuvashalauncher_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"mainClass\": 42}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  version = nuvashalauncher_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"libraries\": {}}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* A rule nobody can obey. */
  version = nuvashalauncher_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"libraries\":"
    " [{\"name\": \"a:b:c\", \"rules\": [{\"action\": \"maybe\"}]}]}", -1,
    &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* An argument without anything to say. */
  version = nuvashalauncher_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"arguments\":"
    " {\"game\": [{\"rules\": [{\"action\": \"allow\"}]}]}}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_MISSING);
  g_clear_error (&error);

  g_print ("ok - a version document with malformed fields is rejected\n");
}

static void
test_rules (void)
{
  g_autoptr (nuvashalauncherVersionDescription) rules =
    fixture_version ("version-rules.json");
  g_autoptr (nuvashalauncherVersionDescription) modern =
    fixture_version ("version-modern.json");
  g_autoptr (nuvashalauncherRuleContext) host = nuvashalauncher_rule_context_new ();
  g_autoptr (nuvashalauncherRuleContext) linux64 =
    nuvashalauncher_rule_context_new_for ("linux", "x86_64", "6.8.0-nuvashalauncher");
  g_autoptr (nuvashalauncherRuleContext) linux32 =
    nuvashalauncher_rule_context_new_for ("linux", "x86", "6.8.0-nuvashalauncher");
  g_autoptr (nuvashalauncherRuleContext) linux_arm =
    nuvashalauncher_rule_context_new_for ("linux", "aarch64", "5.15.0-arm");
  g_autoptr (nuvashalauncherRuleContext) windows =
    nuvashalauncher_rule_context_new_for ("windows", "amd64", "10.0");
  g_autoptr (nuvashalauncherRuleContext) demo =
    nuvashalauncher_rule_context_new_for ("linux", "x86_64", "6.8.0-nuvashalauncher");
  nuvashalauncherArgument *argument;

  /* This machine is Linux, whatever the kernel calls itself. */
  g_assert_cmpstr (host->os_name, ==, "linux");
  g_assert_nonnull (host->os_arch);
  g_assert_false (nuvashalauncher_rule_context_get_feature (host, "is_demo_user"));

  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "linux-only"),
                                      linux64));
  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "windows-only"),
                                       linux64));
  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "osx-only"),
                                       linux64));

  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "x86-only"),
                                       linux64));
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "x86-only"),
                                      linux32));
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "arm64-only"),
                                      linux_arm));
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "aarch64-only"),
                                      linux_arm));
  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "arm64-only"),
                                       linux64));

  /* Rules without conditions always match, and the last match wins. */
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "unconditional"),
                                      linux64));
  g_assert_false (
    nuvashalauncher_library_allowed (find_library (rules, "allow-then-disallow"),
                         linux64));
  g_assert_true (
    nuvashalauncher_library_allowed (find_library (rules, "disallow-osx-then-allow"),
                         linux64));

  /* Features are off until somebody turns them on. */
  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "demo-only"),
                                       linux64));
  nuvashalauncher_rule_context_set_feature (demo, "is_demo_user", TRUE);
  g_assert_true (nuvashalauncher_rule_context_get_feature (demo, "is_demo_user"));
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "demo-only"),
                                      demo));
  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "not-demo"),
                                       demo));
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "not-demo"),
                                      linux64));

  /* os.version is a regular expression over the kernel version. */
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "kernel-6-only"),
                                      linux64));
  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "kernel-6-only"),
                                       linux_arm));

  /* No rules at all means nothing took the version away. */
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "no-rules"),
                                      linux64));
  g_assert_true (nuvashalauncher_rules_allow (NULL, linux64));

  /* The same rule on another machine: the matcher compares, it does
   * not simply refuse everything but Linux. */
  g_assert_true (nuvashalauncher_library_allowed (find_library (rules, "windows-only"),
                                      windows));
  g_assert_false (nuvashalauncher_library_allowed (find_library (rules, "linux-only"),
                                       windows));

  /* Arguments carry their own rules. */
  argument = g_ptr_array_index (modern->game_arguments, 8);
  g_assert_false (nuvashalauncher_argument_allowed (argument, linux64));
  g_assert_true (nuvashalauncher_argument_allowed (argument, demo));

  argument = g_ptr_array_index (modern->jvm_arguments, 2);
  g_assert_true (nuvashalauncher_argument_allowed (argument, linux64));

  /* Turning a feature back off puts it where it started. */
  nuvashalauncher_rule_context_set_feature (demo, "is_demo_user", FALSE);
  g_assert_false (nuvashalauncher_rule_context_get_feature (demo, "is_demo_user"));

  g_print ("ok - rules answer for linux, for the architecture and for features\n");
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  test_manifest_parsing ();
  test_manifest_malformed ();
  test_modern_version ();
  test_legacy_version ();
  test_missing_fields ();
  test_malformed_fields ();
  test_rules ();
  return 0;
}
