/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for aml-minecraft-versions.
 *
 * Parsing and rule evaluation run against the JSON documents in
 * tests/fixtures. Fetching runs against a tiny libsoup server on the
 * loopback interface standing in for Mojang, so the tests need neither
 * the internet nor a display. The cache the module writes lives below
 * a temporary XDG_CACHE_HOME, set before anything can ask for the
 * real one. */

#include "aml-minecraft-versions.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#ifndef AML_TEST_FIXTURES
#define AML_TEST_FIXTURES "tests/fixtures"
#endif

/* The cache these tests write into. g_get_user_cache_dir() remembers
 * its first answer, so the environment is set before anything can ask
 * for the real one; cache_start() below does that. */
static char *cache_home;

/* ---------------------------------------------------------- *
 * Fixture documents
 * ---------------------------------------------------------- */

static char *
fixture_text (const char *name)
{
  g_autofree char *path = g_build_filename (AML_TEST_FIXTURES, name, NULL);
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("the fixture \"%s\" cannot be read: %s", name, error->message);

  return contents;
}

static AmlVersionDescription *
fixture_version (const char *name)
{
  g_autofree char *text = fixture_text (name);
  g_autoptr (GError) error = NULL;
  AmlVersionDescription *description;

  description = aml_version_description_new_from_json (text, -1, &error);

  if (description == NULL)
    g_error ("the fixture \"%s\" does not parse: %s", name, error->message);

  return description;
}

static const AmlLibrary *
find_library (AmlVersionDescription *description, const char *name)
{
  for (guint i = 0; i < description->libraries->len; i++)
    {
      AmlLibrary *library = g_ptr_array_index (description->libraries, i);

      if (g_str_equal (library->name, name))
        return library;
    }

  g_error ("there is no library named \"%s\"", name);
  return NULL;
}

static char *
hash_of (const void *data, gsize length)
{
  return g_compute_checksum_for_data (G_CHECKSUM_SHA1, data, length);
}

/* ---------------------------------------------------------- *
 * Parsing the manifest
 * ---------------------------------------------------------- */

static void
test_manifest_parsing (void)
{
  g_autofree char *text = fixture_text ("manifest.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionManifest) manifest = NULL;
  AmlMcVersionType type = AML_MC_TYPE_RELEASE;
  const AmlVersionEntry *entry;

  manifest = aml_version_manifest_new_from_json (text, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (manifest);

  g_assert_cmpstr (manifest->latest_release, ==, "1.21.1");
  g_assert_cmpstr (manifest->latest_snapshot, ==, "24w33one");
  g_assert_cmpuint (manifest->entries->len, ==, 5);

  entry = g_ptr_array_index (manifest->entries, 0);
  g_assert_cmpstr (entry->id, ==, "24w33one");
  g_assert_cmpint (entry->type, ==, AML_MC_TYPE_SNAPSHOT);
  g_assert_cmpstr (entry->sha1, ==,
                   "655c2da7d6815e84864a0f9a055c90e36337b0b4");
  g_assert_cmpstr (entry->release_time, ==, "2024-08-15T10:00:00+00:00");
  g_assert_true (g_str_has_prefix (entry->url,
                                   "https://piston-meta.mojang.com/"));

  entry = aml_version_manifest_find (manifest, "b1.7.3");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->type, ==, AML_MC_TYPE_OLD_BETA);

  entry = aml_version_manifest_find (manifest, "rd-132211");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->type, ==, AML_MC_TYPE_OLD_ALPHA);

  g_assert_null (aml_version_manifest_find (manifest, "not-a-version"));

  /* The type strings are the four Mojang writes, no others. */
  g_assert_true (aml_mc_version_type_from_string ("old_beta", &type));
  g_assert_cmpint (type, ==, AML_MC_TYPE_OLD_BETA);
  g_assert_false (aml_mc_version_type_from_string ("beta", &type));
  g_assert_false (aml_mc_version_type_from_string (NULL, &type));
  g_assert_cmpstr (aml_mc_version_type_to_string (AML_MC_TYPE_RELEASE), ==,
                   "release");
  g_assert_null (aml_mc_version_type_to_string ((AmlMcVersionType) 99));

  g_print ("ok - the manifest lists releases, snapshots and the old versions\n");
}

static void
test_manifest_malformed (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionManifest) manifest = NULL;
  g_autofree char *incomplete = fixture_text ("manifest-incomplete.json");

  /* Not JSON at all. */
  manifest = aml_version_manifest_new_from_json ("{\"latest\":", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* JSON, but not the shape of a manifest. */
  manifest = aml_version_manifest_new_from_json ("[]", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* Nothing says which version is the latest. */
  manifest = aml_version_manifest_new_from_json ("{\"versions\": []}", -1,
                                                 &error);
  g_assert_null (manifest);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING);
  g_clear_error (&error);

  /* A latest without its snapshot. */
  manifest = aml_version_manifest_new_from_json (
    "{\"latest\": {\"release\": \"1.21.1\"}, \"versions\": []}", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING);
  g_clear_error (&error);

  /* An entry without a sha1 cannot be verified, so it is not an
   * entry. */
  manifest = aml_version_manifest_new_from_json (incomplete, -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING);
  g_clear_error (&error);

  /* A type outside the four. */
  manifest = aml_version_manifest_new_from_json (
    "{\"latest\": {\"release\": \"x\", \"snapshot\": \"y\"},"
    " \"versions\": [{\"id\": \"x\", \"type\": \"beta\","
    " \"url\": \"https://example.invalid/x.json\","
    " \"sha1\": \"1111111111111111111111111111111111111111\","
    " \"releaseTime\": \"2024-01-01T00:00:00+00:00\"}]}", -1, &error);
  g_assert_null (manifest);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  g_print ("ok - a manifest that is not a manifest is rejected\n");
}

/* ---------------------------------------------------------- *
 * One version document, modern and legacy
 * ---------------------------------------------------------- */

static void
test_modern_version (void)
{
  g_autoptr (AmlVersionDescription) version =
    fixture_version ("version-modern.json");
  const AmlDownloadInfo *asset_index;
  const AmlArgument *argument;
  const AmlLibrary *library;

  g_assert_cmpstr (version->id, ==, "1.21.1");
  g_assert_cmpint (version->type, ==, AML_MC_TYPE_RELEASE);
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

  /* The modern arguments, and nothing of the legacy line. */
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
    g_autoptr (GPtrArray) argv = aml_version_description_legacy_argv (version);

    g_assert_cmpuint (argv->len, ==, 0);
  }

  g_print ("ok - a modern version document keeps its arguments and downloads\n");
}

static void
test_legacy_version (void)
{
  g_autoptr (AmlVersionDescription) version =
    fixture_version ("version-legacy.json");
  const AmlLibrary *library;
  g_autoptr (GPtrArray) argv = NULL;

  g_assert_cmpstr (version->id, ==, "1.5.2");
  g_assert_cmpstr (version->main_class, ==,
                   "net.minecraft.launchwrapper.Launch");
  g_assert_false (version->modern_arguments);
  g_assert_cmpuint (version->jvm_arguments->len, ==, 0);
  g_assert_cmpuint (version->game_arguments->len, ==, 0);
  g_assert_nonnull (version->minecraft_arguments);

  argv = aml_version_description_legacy_argv (version);
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
    const AmlDownloadInfo *info =
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

/* ---------------------------------------------------------- *
 * Missing and malformed fields
 * ---------------------------------------------------------- */

static void
test_missing_fields (void)
{
  g_autofree char *incomplete = fixture_text ("version-incomplete.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionDescription) version = NULL;

  /* The identity of a document is what it must carry. */
  version = aml_version_description_new_from_json (incomplete, -1, &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING);
  g_clear_error (&error);

  version = aml_version_description_new_from_json ("{\"id\": \"x\"}", -1,
                                                   &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING);
  g_clear_error (&error);

  g_print ("ok - a version without id or type is rejected\n");
}

static void
test_malformed_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionDescription) version = NULL;

  /* Not JSON. */
  version = aml_version_description_new_from_json ("<html>", -1, &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* JSON that is not an object. */
  version = aml_version_description_new_from_json ("\"1.21.1\"", -1, &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* A type outside the four. */
  version = aml_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"beta\"}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* A field of the wrong shape. */
  version = aml_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"mainClass\": 42}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  version = aml_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"libraries\": {}}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* A rule nobody can obey. */
  version = aml_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"libraries\":"
    " [{\"name\": \"a:b:c\", \"rules\": [{\"action\": \"maybe\"}]}]}", -1,
    &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID);
  g_clear_error (&error);

  /* An argument without anything to say. */
  version = aml_version_description_new_from_json (
    "{\"id\": \"x\", \"type\": \"release\", \"arguments\":"
    " {\"game\": [{\"rules\": [{\"action\": \"allow\"}]}]}}", -1, &error);
  g_assert_null (version);
  g_assert_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING);
  g_clear_error (&error);

  g_print ("ok - a version document with malformed fields is rejected\n");
}

/* ---------------------------------------------------------- *
 * Rules
 * ---------------------------------------------------------- */

static void
test_rules (void)
{
  g_autoptr (AmlVersionDescription) rules =
    fixture_version ("version-rules.json");
  g_autoptr (AmlVersionDescription) modern =
    fixture_version ("version-modern.json");
  g_autoptr (AmlRuleContext) host = aml_rule_context_new ();
  g_autoptr (AmlRuleContext) linux64 =
    aml_rule_context_new_for ("linux", "x86_64", "6.8.0-aml");
  g_autoptr (AmlRuleContext) linux32 =
    aml_rule_context_new_for ("linux", "x86", "6.8.0-aml");
  g_autoptr (AmlRuleContext) linux_arm =
    aml_rule_context_new_for ("linux", "aarch64", "5.15.0-arm");
  g_autoptr (AmlRuleContext) windows =
    aml_rule_context_new_for ("windows", "amd64", "10.0");
  g_autoptr (AmlRuleContext) demo =
    aml_rule_context_new_for ("linux", "x86_64", "6.8.0-aml");
  AmlArgument *argument;

  /* This machine is Linux, whatever the kernel calls itself. */
  g_assert_cmpstr (host->os_name, ==, "linux");
  g_assert_nonnull (host->os_arch);
  g_assert_false (aml_rule_context_get_feature (host, "is_demo_user"));

  g_assert_true (aml_library_allowed (find_library (rules, "linux-only"),
                                      linux64));
  g_assert_false (aml_library_allowed (find_library (rules, "windows-only"),
                                       linux64));
  g_assert_false (aml_library_allowed (find_library (rules, "osx-only"),
                                       linux64));

  /* 32 bit and 64 bit are different machines, and the two names of
   * the arm machine are the same one. */
  g_assert_false (aml_library_allowed (find_library (rules, "x86-only"),
                                       linux64));
  g_assert_true (aml_library_allowed (find_library (rules, "x86-only"),
                                      linux32));
  g_assert_true (aml_library_allowed (find_library (rules, "arm64-only"),
                                      linux_arm));
  g_assert_true (aml_library_allowed (find_library (rules, "aarch64-only"),
                                      linux_arm));
  g_assert_false (aml_library_allowed (find_library (rules, "arm64-only"),
                                       linux64));

  /* Rules without conditions always match, and the last match wins. */
  g_assert_true (aml_library_allowed (find_library (rules, "unconditional"),
                                      linux64));
  g_assert_false (
    aml_library_allowed (find_library (rules, "allow-then-disallow"),
                         linux64));
  g_assert_true (
    aml_library_allowed (find_library (rules, "disallow-osx-then-allow"),
                         linux64));

  /* Features are off until somebody turns them on. */
  g_assert_false (aml_library_allowed (find_library (rules, "demo-only"),
                                       linux64));
  aml_rule_context_set_feature (demo, "is_demo_user", TRUE);
  g_assert_true (aml_rule_context_get_feature (demo, "is_demo_user"));
  g_assert_true (aml_library_allowed (find_library (rules, "demo-only"),
                                      demo));
  g_assert_false (aml_library_allowed (find_library (rules, "not-demo"),
                                       demo));
  g_assert_true (aml_library_allowed (find_library (rules, "not-demo"),
                                      linux64));

  /* os.version is a regular expression over the kernel version. */
  g_assert_true (aml_library_allowed (find_library (rules, "kernel-6-only"),
                                      linux64));
  g_assert_false (aml_library_allowed (find_library (rules, "kernel-6-only"),
                                       linux_arm));

  /* No rules at all means nothing took the version away. */
  g_assert_true (aml_library_allowed (find_library (rules, "no-rules"),
                                      linux64));
  g_assert_true (aml_rules_allow (NULL, linux64));

  /* The same rule on another machine: the matcher compares, it does
   * not simply refuse everything but Linux. */
  g_assert_true (aml_library_allowed (find_library (rules, "windows-only"),
                                      windows));
  g_assert_false (aml_library_allowed (find_library (rules, "linux-only"),
                                       windows));

  /* Arguments carry their own rules. */
  argument = g_ptr_array_index (modern->game_arguments, 8);
  g_assert_false (aml_argument_allowed (argument, linux64));
  g_assert_true (aml_argument_allowed (argument, demo));

  argument = g_ptr_array_index (modern->jvm_arguments, 2);
  g_assert_true (aml_argument_allowed (argument, linux64));

  /* Turning a feature back off puts it where it started. */
  aml_rule_context_set_feature (demo, "is_demo_user", FALSE);
  g_assert_false (aml_rule_context_get_feature (demo, "is_demo_user"));

  g_print ("ok - rules answer for linux, for the architecture and for features\n");
}

/* ---------------------------------------------------------- *
 * Merging a profile with the version it inherits from
 * ---------------------------------------------------------- */

static void
test_inheritance_merge (void)
{
  g_autoptr (AmlVersionDescription) parent =
    fixture_version ("version-parent.json");
  g_autoptr (AmlVersionDescription) child =
    fixture_version ("version-child.json");
  g_autoptr (AmlVersionDescription) grandchild =
    fixture_version ("version-grandchild.json");
  g_autoptr (AmlVersionDescription) merged = NULL;
  g_autoptr (AmlVersionDescription) higher = NULL;
  const AmlLibrary *library;
  const AmlArgument *argument;

  merged = aml_version_description_merge (parent, child);
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
  higher = aml_version_description_merge (child, grandchild);
  g_assert_nonnull (higher);
  g_assert_cmpstr (higher->id, ==, "my-profile");
  g_assert_cmpstr (higher->inherits_from, ==, "1.21.1");
  g_assert_cmpstr (higher->main_class, ==,
                   "net.fabricmc.loader.impl.launch.knot.KnotClient");
  g_assert_cmpuint (higher->libraries->len, ==, 3);
  g_assert_cmpuint (higher->jvm_arguments->len, ==, 1);
  g_assert_cmpuint (higher->game_arguments->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (
                     ((AmlArgument *) g_ptr_array_index (
                        higher->game_arguments, 0))->values, 0),
                   ==, "--quickPlayPath");

  g_print ("ok - a profile is merged over the version it inherits from\n");
}

/* ---------------------------------------------------------- *
 * Fetching: one server standing in for Mojang
 * ---------------------------------------------------------- */

typedef struct
{
  const char *id;
  const char *type;
  const char *path;    /* where the document is served */
  const char *fixture; /* the file holding it, or NULL */
  const char *body;    /* an inline document, or NULL */
} ServedVersion;

static const ServedVersion served_versions[] = {
  { "1.21.1", "release", "/1.21.1.json", "version-parent.json", NULL },
  { "fabric-loader-0.16.9-1.21.1", "release", "/fabric.json",
    "version-child.json", NULL },
  { "my-profile", "release", "/profile.json", "version-grandchild.json",
    NULL },
  { "cycle-a", "release", "/cycle-a.json", NULL,
    "{\"id\": \"cycle-a\", \"type\": \"release\", \"mainClass\": \"a\","
    " \"inheritsFrom\": \"cycle-b\"}" },
  { "cycle-b", "release", "/cycle-b.json", NULL,
    "{\"id\": \"cycle-b\", \"type\": \"release\", \"mainClass\": \"b\","
    " \"inheritsFrom\": \"cycle-a\"}" },
};

typedef struct
{
  SoupServer *server;
  char *base;       /* "http://127.0.0.1:port", no trailing slash */
  GHashTable *files; /* path -> GString * */
  guint hits;       /* requests seen, reset by every test */
} Fixture;

static Fixture fixture;

static char *
served_body (const ServedVersion *version)
{
  if (version->body != NULL)
    return g_strdup (version->body);

  return fixture_text (version->fixture);
}

static void
free_string (gpointer data)
{
  g_string_free (data, TRUE);
}

static void
serve (const char *path, const void *data, gsize length)
{
  g_hash_table_replace (fixture.files, g_strdup (path),
                        g_string_new_len (data, length));
}

static void
server_handler (SoupServer *server, SoupServerMessage *message,
                const char *path, GHashTable *query, gpointer user_data)
{
  Fixture *self = user_data;
  GString *body;

  (void) server;
  (void) query;

  self->hits++;

  body = g_hash_table_lookup (self->files, path);

  if (body == NULL)
    {
      soup_server_message_set_status (message, SOUP_STATUS_NOT_FOUND, NULL);
      return;
    }

  soup_server_message_set_status (message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (message, "application/json",
                                    SOUP_MEMORY_COPY, body->str, body->len);
}

/* The manifest the way Mojang writes it, with the address of the test
 * server in every entry. */
static char *
manifest_json (void)
{
  GString *json = g_string_new (
    "{\"latest\": {\"release\": \"1.21.1\", \"snapshot\": \"1.21.1\"},"
    " \"versions\": [");

  for (guint i = 0; i < G_N_ELEMENTS (served_versions); i++)
    {
      const ServedVersion *version = &served_versions[i];
      g_autofree char *body = served_body (version);
      g_autofree char *sha1 = hash_of (body, strlen (body));

      g_string_append_printf (
        json,
        "%s{\"id\": \"%s\", \"type\": \"%s\", \"url\": \"%s%s\","
        " \"releaseTime\": \"2024-01-01T00:00:00+00:00\","
        " \"sha1\": \"%s\"}",
        i > 0 ? "," : "", version->id, version->type, fixture.base,
        version->path, sha1);
    }

  g_string_append (json, "]}");

  return g_string_free (json, FALSE);
}

static char *
server_url (const char *path)
{
  return g_strconcat (fixture.base, path, NULL);
}

static void
fixture_start (void)
{
  g_autoptr (GError) error = NULL;
  GSList *uris = NULL;
  char *uri_string = NULL;

  fixture.files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                         free_string);
  fixture.server = soup_server_new ("server-header", "aml-test", NULL);

  if (!soup_server_listen_local (fixture.server, 0, 0, &error))
    g_error ("the test server cannot listen: %s", error->message);

  soup_server_add_handler (fixture.server, "/", server_handler, &fixture,
                           NULL);

  uris = soup_server_get_uris (fixture.server);

  if (uris == NULL)
    g_error ("the test server has no address");

  uri_string = g_uri_to_string (uris->data);
  fixture.base = g_strdup (uri_string);

  /* The address keeps its trailing slash, every path in the tests
   * starts with a slash of its own. */
  if (fixture.base[0] != '\0')
    {
      gsize length = strlen (fixture.base);

      if (fixture.base[length - 1] == '/')
        fixture.base[length - 1] = '\0';
    }

  g_free (uri_string);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  /* Everything the server can be asked for. */
  {
    g_autofree char *manifest = manifest_json ();

    serve ("/manifest.json", manifest, strlen (manifest));

    for (guint i = 0; i < G_N_ELEMENTS (served_versions); i++)
      {
        g_autofree char *body = served_body (&served_versions[i]);

        serve (served_versions[i].path, body, strlen (body));
      }
  }
}

static void
fixture_stop (void)
{
  g_clear_pointer (&fixture.files, g_hash_table_unref);
  g_clear_object (&fixture.server);
  g_clear_pointer (&fixture.base, g_free);
}

/* ---------------------------------------------------------- *
 * One run of the main loop with one request in flight
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  guint pending;
  guint watchdog;
  gboolean timed_out;
  GCancellable *cancellable;
} AsyncRun;

typedef struct
{
  AsyncRun *run;
  GError *error;
  AmlVersionManifest *manifest;
  AmlVersionDescription *description;
  AmlTaskScope *operation;
} AsyncCall;

static void
call_finished (AsyncCall *call)
{
  aml_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

/* A callback that never arrives fails the test instead of hanging. */
static gboolean
async_timeout (gpointer user_data)
{
  AsyncRun *run = user_data;

  run->timed_out = TRUE;
  g_main_loop_quit (run->loop);

  return G_SOURCE_REMOVE;
}

static void
async_run_init (AsyncRun *run)
{
  run->loop = g_main_loop_new (NULL, FALSE);
  run->cancellable = g_cancellable_new ();
}

static void
async_run_wait (AsyncRun *run)
{
  run->watchdog = g_timeout_add_seconds (30, async_timeout, run);

  if (run->pending > 0)
    g_main_loop_run (run->loop);

  if (!run->timed_out)
    g_source_remove (run->watchdog);

  g_assert_false (run->timed_out);
}

static void
async_run_clear (AsyncRun *run)
{
  if (run->loop != NULL)
    {
      g_main_loop_unref (run->loop);
      run->loop = NULL;
    }

  g_clear_object (&run->cancellable);
}

static void
async_call_clear (AsyncCall *call)
{
  g_clear_error (&call->error);
  g_clear_object (&call->manifest);
  g_clear_object (&call->description);
}

static void
manifest_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  AsyncCall *call = user_data;

  call->manifest = aml_version_manifest_fetch_finish (result, &call->error);
  call_finished (call);
}

static void
load_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  AsyncCall *call = user_data;

  call->description = aml_version_load_finish (result, &call->error);
  call_finished (call);
}

static void
call_manifest (AsyncRun *run, AsyncCall *call, const char *url)
{
  call->run = run;
  run->pending++;

  call->operation = aml_version_manifest_fetch_async (url, manifest_finished,
                                                      call);
  aml_task_scope_follow_cancellable (call->operation, run->cancellable);
}

static void
call_load (AsyncRun *run, AsyncCall *call, AmlVersionManifest *manifest,
           const char *version_id)
{
  call->run = run;
  run->pending++;

  call->operation = aml_version_load_async (manifest, version_id,
                                             load_finished, call);
  aml_task_scope_follow_cancellable (call->operation, run->cancellable);
}

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

static char *
cache_file (const char *name)
{
  g_autofree char *directory =
    g_build_filename (g_get_user_cache_dir (), "AML", "versions", NULL);

  return g_build_filename (directory, name, NULL);
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

/* The manifest of the tests, parsed without going through the
 * network. */
static AmlVersionManifest *
local_manifest (void)
{
  g_autofree char *json = manifest_json ();
  g_autoptr (GError) error = NULL;
  AmlVersionManifest *manifest;

  manifest = aml_version_manifest_new_from_json (json, -1, &error);

  if (manifest == NULL)
    g_error ("the manifest of the test server does not parse: %s",
             error->message);

  return manifest;
}

static void
test_version_load (void)
{
  g_autoptr (AmlVersionManifest) manifest = local_manifest ();
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
  g_autoptr (AmlVersionManifest) manifest = local_manifest ();
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);

  /* Nothing is fetched for a version the manifest does not know. */
  call_load (&run, &call, manifest, "not-a-version");
  async_run_wait (&run);

  g_assert_null (call.description);
  g_assert_error (call.error, AML_MC_ERROR, AML_MC_ERROR_NOT_FOUND);
  g_assert_cmpuint (fixture.hits, ==, 0);
  async_call_clear (&call);

  /* A pair of documents that inherit from each other end as soon as
   * the second one points back. */
  call_load (&run, &call, manifest, "cycle-a");
  async_run_wait (&run);

  g_assert_null (call.description);
  g_assert_error (call.error, AML_MC_ERROR, AML_MC_ERROR_CYCLE);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);

  /* The same again, this time from the copies the first attempt
   * left behind. */
  call_load (&run, &call, manifest, "cycle-a");
  async_run_wait (&run);

  g_assert_null (call.description);
  g_assert_error (call.error, AML_MC_ERROR, AML_MC_ERROR_CYCLE);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);

  async_run_clear (&run);

  g_print ("ok - an unknown version and an inheritance cycle both fail\n");
}

/* ---------------------------------------------------------- *
 * The cache directory of these tests
 * ---------------------------------------------------------- */

/* g_get_user_cache_dir() remembers its first answer, so the
 * environment is set before anything can ask for it. */
static void
cache_start (void)
{
  g_autoptr (GError) error = NULL;

  cache_home = g_dir_make_tmp ("aml-versions-cache-XXXXXX", &error);

  if (cache_home == NULL)
    g_error ("the cache directory of the test cannot be made: %s",
             error->message);

  g_setenv ("XDG_CACHE_HOME", cache_home, TRUE);
}

static void
cache_stop (void)
{
  g_autofree char *directory =
     g_build_filename (cache_home, "AML", "versions", NULL);
  g_autofree char *aml_directory =
    g_build_filename (cache_home, "AML", NULL);
  g_autoptr (GDir) dir = g_dir_open (directory, 0, NULL);

  if (dir != NULL)
    {
      const char *name;

      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *path = g_build_filename (directory, name, NULL);

          g_remove (path);
        }
    }

  g_rmdir (directory);
  g_rmdir (aml_directory);
  g_rmdir (cache_home);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  cache_start ();
  fixture_start ();

  test_manifest_parsing ();
  test_manifest_malformed ();
  test_modern_version ();
  test_legacy_version ();
  test_missing_fields ();
  test_malformed_fields ();
  test_rules ();
  test_inheritance_merge ();
  test_manifest_fetch ();
  test_version_load ();
  test_version_load_errors ();

  fixture_stop ();
  cache_stop ();

  return EXIT_SUCCESS;
}
