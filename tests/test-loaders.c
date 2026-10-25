/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvasha-loaders.
 *
 * Every loader answers with a document of its own, and what nuvasha has to
 * get right is reading it: which versions a loader has for one Minecraft
 * version, which of those it calls stable, and the document those
 * versions are, whose libraries publish a maven address instead of a
 * download.
 *
 * The answers come from the fixtures in tests/fixtures, trimmed from
 * what the loaders themselves published, so nothing here needs the
 * network. */

#include "nuvasha-loaders.h"

#include "nuvasha-download.h"
#include "nuvasha-minecraft-resolve.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#ifndef nuvasha_TEST_FIXTURES
#define nuvasha_TEST_FIXTURES "tests/fixtures"
#endif

static const NuvashaLoaderVersion *
version_at (GPtrArray *versions, guint index)
{
  g_assert_cmpuint (index, <, versions->len);

  return g_ptr_array_index (versions, index);
}

/* ---------------------------------------------------------- *
 * The loaders that are in the lists
 * ---------------------------------------------------------- */

static void
test_supported_loaders (void)
{
  const NuvashaLoader *loaders = nuvasha_loaders_supported ();
  guint count = nuvasha_loaders_supported_count ();
  gboolean vanilla = FALSE;
  gboolean fabric = FALSE;

  /* Vanilla is what an instance is without a loader, so it is always in
     the list. */
  g_assert_cmpuint (count, >, 0);

  for (guint i = 0; i < count; i++)
    {
      g_assert_true (nuvasha_loader_is_supported (loaders[i]));
      g_assert_nonnull (nuvasha_loader_display_name (loaders[i]));

      if (loaders[i] == nuvasha_LOADER_VANILLA)
        vanilla = TRUE;

      if (loaders[i] == nuvasha_LOADER_FABRIC)
        fabric = TRUE;
    }

  g_assert_true (vanilla);
  g_assert_true (fabric);

  g_assert_cmpstr (nuvasha_loader_display_name (nuvasha_LOADER_VANILLA), ==, "Vanilla");
  g_assert_cmpstr (nuvasha_loader_display_name (nuvasha_LOADER_FABRIC), ==, "Fabric");
  g_assert_cmpstr (nuvasha_loader_display_name (nuvasha_LOADER_QUILT), ==, "Quilt");

  g_print ("ok - the loader lists hold the loaders nuvasha installs\n");
}

static void
test_fabric_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/fabric-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = nuvasha_loaders_versions_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (versions);
  g_assert_cmpuint (versions->len, ==, 5);

  /* Newest first, whatever order the loader answered in: a list a user
     reads starts with the version to install. */
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "0.19.5");
  g_assert_cmpstr (version_at (versions, 1)->version, ==, "0.19.4");
  g_assert_cmpstr (version_at (versions, 2)->version, ==, "0.16.3");
  g_assert_cmpstr (version_at (versions, 3)->version, ==, "0.10.7+build.214");
  g_assert_cmpstr (version_at (versions, 4)->version, ==, "0.9.3+build.207");

   /* Old numeric releases remain stable even when the service marks them
      as old; build/pre-release suffixes are unstable. */
   g_assert_true (version_at (versions, 0)->stable);
   g_assert_true (version_at (versions, 1)->stable);
   g_assert_true (version_at (versions, 2)->stable);
   g_assert_false (version_at (versions, 3)->stable);

  g_print ("ok - a Fabric list is read, newest first, with what is stable\n");
}

static void
test_quilt_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/quilt-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = nuvasha_loaders_versions_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (versions);
  g_assert_cmpuint (versions->len, ==, 4);

  /* Quilt answers in no order at all, so the list is sorted here. */
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "0.30.2-beta.1");
  g_assert_cmpstr (version_at (versions, 1)->version, ==, "0.30.1");
  g_assert_cmpstr (version_at (versions, 2)->version, ==, "0.28.1");
  g_assert_cmpstr (version_at (versions, 3)->version, ==, "0.18.4-pre.3");

  /* Quilt says nothing about stability, so a version that is not a final
     release is the one that is not stable. */
  g_assert_false (version_at (versions, 0)->stable);
  g_assert_true (version_at (versions, 1)->stable);
  g_assert_true (version_at (versions, 2)->stable);
  g_assert_false (version_at (versions, 3)->stable);

  g_print ("ok - a Quilt list is read and a pre-release is not stable\n");
}

static void
test_empty_and_broken_answers (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  /* A loader with no version for that Minecraft version answers with an
     empty list, which is not the same as an error the user has to read
     twice. */
  versions = nuvasha_loaders_versions_from_json ("[]", -1, &error);
  g_assert_null (versions);
  g_assert_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_VERSIONS);
  g_clear_error (&error);

  /* Something that is not a list at all is refused as well. */
  versions = nuvasha_loaders_versions_from_json ("{\"error\": \"nope\"}", -1,
                                             &error);
  g_assert_null (versions);
  g_assert_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_VERSIONS);
  g_clear_error (&error);

  /* An entry that says nothing about a version is left out instead of
     becoming an empty row. */
  versions = nuvasha_loaders_versions_from_json (
    "[{\"loader\": {}}, {\"loader\": {\"version\": \"0.19.5\"}}]", -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 1);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "0.19.5");

  g_print ("ok - an answer with nothing in it is refused, not shown\n");
}

/* ---------------------------------------------------------- *
 * The loaders that publish an installer
 * ---------------------------------------------------------- */

static void
test_which_loaders_publish_an_installer (void)
{
  /* The two that serve a version document of their own and the two that
     publish an installer are installed in different ways, and the launcher
     asks which it is dealing with. */
  g_assert_false (nuvasha_loader_publishes_an_installer (nuvasha_LOADER_VANILLA));
  g_assert_false (nuvasha_loader_publishes_an_installer (nuvasha_LOADER_FABRIC));
  g_assert_false (nuvasha_loader_publishes_an_installer (nuvasha_LOADER_QUILT));
  g_assert_true (nuvasha_loader_publishes_an_installer (nuvasha_LOADER_NEOFORGE));
  g_assert_true (nuvasha_loader_publishes_an_installer (nuvasha_LOADER_FORGE));

  g_print ("ok - the loaders that publish an installer are told apart\n");
}

/* Which versions of a loader run which Minecraft version: both name their
   versions after the game they run, Forge with a dash between the two and
   NeoForge with a dot. */
static void
test_compatibility_filtering (void)
{
  /* Forge: "1.20.1-47.2.0" runs 1.20.1 and nothing else, so a longer game
     version is not a prefix match. */
  g_assert_true (nuvasha_loaders_version_runs_game (nuvasha_LOADER_FORGE,
                                                "1.20.1-47.2.0", "1.20.1"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_FORGE,
                                                 "1.20.1-47.2.0", "1.20"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_FORGE,
                                                 "1.20.1-47.2.0", "1.20.2"));
  g_assert_true (nuvasha_loaders_version_runs_game (nuvasha_LOADER_FORGE,
                                                "1.21-51.0.33", "1.21"));

  /* NeoForge: "26.3.0.51-beta" runs 26.3, and a version of another game
     does not. */
  g_assert_true (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                "26.3.0.51-beta", "26.3"));
  g_assert_true (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                "21.1.256", "21.1"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                 "21.1.256", "21.11"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                 "20.2.3-beta", "26.3"));

  /* Modern NeoForge drops the leading "1." of a Minecraft version and
     writes a missing patch as 0, so the question is asked with the real
     Minecraft version the user picked. */
  g_assert_true (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                "21.1.256", "1.21.1"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                 "21.1.256", "1.21"));
  g_assert_true (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                "21.0.5", "1.21"));
  g_assert_true (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                "20.2.3-beta", "1.20.2"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                 "20.2.3-beta", "1.20.1"));

  /* A loader that publishes no installer is not asked the question: its
     versions are its own and name no game. */
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_FABRIC,
                                                 "0.19.5", "26.3"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE, NULL,
                                                 "26.3"));
  g_assert_false (nuvasha_loaders_version_runs_game (nuvasha_LOADER_NEOFORGE,
                                                 "26.3.0.51-beta", NULL));

  g_print ("ok - the versions of a loader are told apart by their names\n");
}

/* The versions of NeoForge for one Minecraft version, out of everything
   its maven holds. */
static void
test_neoforge_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/neoforge-versions.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  /* The maven holds every version NeoForge ever published, for every game;
     only the ones named after the game that was asked for are versions of
     it. */
  versions = nuvasha_loaders_maven_versions_from_json (nuvasha_LOADER_NEOFORGE, "26.3",
                                                    json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (versions);
  g_assert_cmpuint (versions->len, ==, 3);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "26.3.0.51-beta");
  g_assert_cmpstr (version_at (versions, 2)->version, ==, "26.3.0.49-beta");

  /* Every NeoForge of this game version is a beta, which is what the
     suffix says. */
  g_assert_false (version_at (versions, 0)->stable);
  g_assert_false (version_at (versions, 1)->stable);
  g_assert_false (version_at (versions, 2)->stable);

  /* Another game version out of the same answer. */
  g_clear_pointer (&versions, g_ptr_array_unref);
  versions = nuvasha_loaders_maven_versions_from_json (nuvasha_LOADER_NEOFORGE,
                                                    "21.1", json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 3);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "21.1.256");

  g_assert_true (version_at (versions, 0)->stable);
  g_assert_true (version_at (versions, 2)->stable);

  /* The same list asked for with the real Minecraft version the user
     picked, which NeoForge spells without the leading "1.". */
  g_clear_pointer (&versions, g_ptr_array_unref);
  versions = nuvasha_loaders_maven_versions_from_json (nuvasha_LOADER_NEOFORGE,
                                                   "1.21.1", json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 3);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "21.1.256");

  /* A game no version of NeoForge runs. */
  g_clear_pointer (&versions, g_ptr_array_unref);
  versions = nuvasha_loaders_maven_versions_from_json (nuvasha_LOADER_NEOFORGE,
                                                    "1.20.1", json, -1,
                                                    &error);
  g_assert_null (versions);
  g_assert_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_VERSIONS);

  g_print ("ok - a NeoForge maven is read and filtered by game version\n");
}

/* The same for Forge, whose names carry a dash and whose releases are
   releases even with a dash in the name. */
static void
test_forge_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/forge-versions.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = nuvasha_loaders_maven_versions_from_json (nuvasha_LOADER_FORGE, "1.20.1",
                                                    json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (versions);
  g_assert_cmpuint (versions->len, ==, 3);

  /* Newest first, and the numbers compared as numbers: 47.10 would be
     newer than 47.9, which sorting them as text would get wrong. */
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "1.20.1-47.3.0");
  g_assert_cmpstr (version_at (versions, 1)->version, ==, "1.20.1-47.2.0");
  g_assert_cmpstr (version_at (versions, 2)->version, ==, "1.20.1-47.1.7");

  /* The dash in a Forge version is what joins the game version to its own
     numbers, so these are releases and not pre-releases. */
  g_assert_true (version_at (versions, 0)->stable);
  g_assert_true (version_at (versions, 2)->stable);

  g_clear_pointer (&versions, g_ptr_array_unref);
  versions = nuvasha_loaders_maven_versions_from_json (nuvasha_LOADER_FORGE, "1.16.5",
                                                    json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 1);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "1.16.5-36.2.34");

  g_print ("ok - a Forge maven is read and filtered by game version\n");
}

/* An answer that is not a list of versions is refused, whatever the
   loader. */
static void
test_a_maven_that_answers_nothing (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  versions = nuvasha_loaders_maven_versions_from_json (
    nuvasha_LOADER_FORGE, "1.20.1", "{\"status\": 404}", -1, &error);
  g_assert_null (versions);
  g_assert_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_VERSIONS);

  g_print ("ok - a maven that answers nothing is refused\n");
}

/* ---------------------------------------------------------- *
 * The document of a loader version
 * ---------------------------------------------------------- */

static const NuvashaLibrary *
find_library (NuvashaVersionDescription *description, const char *name)
{
  for (guint i = 0; i < description->libraries->len; i++)
    {
      const NuvashaLibrary *library = g_ptr_array_index (description->libraries, i);

      if (g_strcmp0 (library->name, name) == 0)
        return library;
    }

  return NULL;
}

static void
test_fabric_profile (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaVersionDescription) profile = NULL;
  g_autoptr (NuvashaVersionDescription) vanilla = NULL;
  g_autoptr (NuvashaVersionDescription) merged = NULL;
  const NuvashaLibrary *library = NULL;
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

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/fabric-profile.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  profile = nuvasha_loaders_profile_from_json (json, -1, &error);
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
    const NuvashaArgument *argument = g_ptr_array_index (profile->jvm_arguments, 0);

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
  vanilla = nuvasha_version_description_new_from_json (vanilla_document, -1,
                                                   &error);
  g_assert_no_error (error);
  g_assert_nonnull (vanilla);

  merged = nuvasha_version_description_merge (vanilla, profile);
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
    const NuvashaLibrary *first = g_ptr_array_index (merged->libraries, 0);

    g_assert_cmpstr (first->name, ==, "org.lwjgl:lwjgl:3.3.3");
  }
  g_assert_cmpuint (merged->jvm_arguments->len, ==, 3);
  g_assert_cmpuint (merged->game_arguments->len, ==, 2);

  g_print ("ok - a Fabric profile becomes a document that merges over it\n");
}

/* Quilt publishes its libraries without a hash at all, so the document
   has to be usable even when nothing can be verified. */
static void
test_quilt_profile (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaVersionDescription) profile = NULL;
  const NuvashaLibrary *library = NULL;

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/quilt-profile.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  profile = nuvasha_loaders_profile_from_json (json, -1, &error);
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
  g_assert_null (nuvasha_loaders_profile_from_json (
    "{\"id\": \"x\", \"type\": \"release\","
    " \"libraries\": [{\"name\": \"no:where:1.0\"}]}", -1, &error));
  g_assert_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_MAVEN);
  g_clear_error (&error);

  /* Coordinates that make no path are refused as well, because the path
     they would write into the library directory has to be a name. */
  g_assert_null (nuvasha_loaders_profile_from_json (
    "{\"id\": \"x\", \"type\": \"release\","
    " \"libraries\": [{\"name\": \"../evil:evil:1.0\","
    "                 \"url\": \"https://example.invalid/\"}]}", -1, &error));
  g_assert_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_MAVEN);

  g_print ("ok - a profile with no usable maven address is refused\n");
}

/* ---------------------------------------------------------- *
 * What a version list shows
 * ---------------------------------------------------------- */

/* The display policy: unstable versions are hidden unless the setting
 * asks for them, a version an instance runs is kept whatever its state,
 * and the newest stable one is preselected. */
static void
test_versions_for_display (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) published = NULL;
  g_autoptr (GPtrArray) shown = NULL;
  guint selected = 0;

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/fabric-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  published = nuvasha_loaders_versions_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (published);

  /* Stable only, and the newest stable version is picked. */
  shown = nuvasha_loaders_versions_for_display (published, FALSE, NULL, &selected);
  g_assert_nonnull (shown);
  g_assert_cmpuint (shown->len, >, 0);
  g_assert_cmpuint (selected, ==, 0);
  g_assert_cmpstr (version_at (shown, selected)->version, ==, "0.19.5");

  for (guint i = 0; i < shown->len; i++)
    g_assert_true (version_at (shown, i)->stable);

  /* Asking for unstable versions shows the whole list. */
  g_clear_pointer (&shown, g_ptr_array_unref);
  shown = nuvasha_loaders_versions_for_display (published, TRUE, NULL, &selected);
  g_assert_cmpuint (shown->len, ==, published->len);
  g_assert_cmpstr (version_at (shown, selected)->version, ==, "0.19.5");

  /* An instance that runs an unstable version keeps it even when the
     setting hides unstable versions, and that version is the pick. */
  g_clear_pointer (&shown, g_ptr_array_unref);
  shown = nuvasha_loaders_versions_for_display (published, FALSE,
                                            "0.10.7+build.214", &selected);
  g_assert_cmpuint (selected, <, shown->len);
  g_assert_cmpstr (version_at (shown, selected)->version, ==,
                   "0.10.7+build.214");

  /* A list that holds only unstable versions, with unstable ones asked
     for, still picks its newest instead of nothing. */
  {
    static const char *const betas_ids[] = { "2.0.0-beta", "1.0.0-beta" };
    g_autoptr (GPtrArray) betas =
      g_ptr_array_new_with_free_func ((GDestroyNotify) nuvasha_loader_version_free);

    for (guint i = 0; i < G_N_ELEMENTS (betas_ids); i++)
      {
        NuvashaLoaderVersion *version = g_new0 (NuvashaLoaderVersion, 1);

        version->version = g_strdup (betas_ids[i]);
        version->stable = FALSE;
        g_ptr_array_add (betas, version);
      }

    g_clear_pointer (&shown, g_ptr_array_unref);
    shown = nuvasha_loaders_versions_for_display (betas, TRUE, NULL, &selected);
    g_assert_cmpuint (shown->len, ==, 2);
    g_assert_cmpuint (selected, ==, 0);
    g_assert_cmpstr (version_at (shown, 0)->version, ==, "2.0.0-beta");
  }

  {
    g_autoptr (GPtrArray) none = g_ptr_array_new ();

    g_clear_pointer (&shown, g_ptr_array_unref);
    shown = nuvasha_loaders_versions_for_display (none, TRUE, NULL, &selected);
    g_assert_cmpuint (shown->len, ==, 0);
    g_assert_cmpuint (selected, ==, G_MAXUINT);
  }

  g_print ("ok - a list hides unstable versions but keeps a chosen one\n");
}

/* Both entry points refuse what the other one is for, and they say which
 * of them to use instead. These answers arrive through the main loop, as
 * every answer of this module does. */

typedef struct
{
  GMainLoop *loop;
  GError *error;
  NuvashaLoaderInstall *install;
  NuvashaVersionDescription *profile;
  GPtrArray *versions;
  guint watchdog;
  NuvashaTaskScope *operation;
} Wait;

static gboolean
watchdog_fired (gpointer user_data)
{
  Wait *wait = user_data;

  g_main_loop_quit (wait->loop);

  return G_SOURCE_REMOVE;
}

static void
on_install_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  Wait *wait = user_data;

  wait->install = nuvasha_loaders_install_finish (result, &wait->error);
  nuvasha_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

static void
on_profile_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  Wait *wait = user_data;

  wait->profile = nuvasha_loaders_profile_finish (result, &wait->error);
  nuvasha_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

static void
on_versions_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                      gpointer user_data)
{
  Wait *wait = user_data;

  wait->versions = nuvasha_loaders_versions_finish (result, &wait->error);
  nuvasha_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

/* A load that was cancelled hands no list to the screen that started it,
   which is what lets the screen drop an answer that belongs to a game
   version or a loader it has moved on from. */
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

  wait.operation = nuvasha_loaders_versions_async (nuvasha_LOADER_FABRIC, "1.20.1",
                                               on_versions_finished, &wait);
  nuvasha_task_scope_follow_cancellable (wait.operation, cancellable);

  g_main_loop_run (wait.loop);
  g_source_remove (wait.watchdog);
  g_main_loop_unref (wait.loop);

  g_assert_null (wait.versions);
  g_assert_nonnull (wait.error);
  g_assert_true (
    g_error_matches (wait.error, nuvasha_DOWNLOAD_ERROR,
                     nuvasha_DOWNLOAD_ERROR_CANCELLED) ||
    g_error_matches (wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
  g_clear_error (&wait.error);

  g_print ("ok - a cancelled version load hands nothing to a screen\n");
}

/* A loader that publishes a version document of its own has no installer,
   and asking for one says so instead of looking for a jar that is not
   there. */
static void
test_install_of_a_loader_that_publishes_none (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaVersionDescription) document = NULL;
  Wait wait = { 0 };

  if (!g_file_get_contents (nuvasha_TEST_FIXTURES "/fabric-profile.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  document = nuvasha_loaders_profile_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (document);

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.watchdog = g_timeout_add_seconds (20, watchdog_fired, &wait);

  wait.operation = nuvasha_loaders_install_async (nuvasha_LOADER_FABRIC, "1.20.1",
                                              "0.19.5", document, "/bin/true",
                                              NULL, NULL, NULL,
                                              on_install_finished, &wait);

  g_main_loop_run (wait.loop);
  g_source_remove (wait.watchdog);
  g_main_loop_unref (wait.loop);

  g_assert_null (wait.install);
  g_assert_error (wait.error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_UNSUPPORTED);
  g_assert_nonnull (strstr (wait.error->message, "Fabric"));
  g_clear_error (&wait.error);

  g_print ("ok - a loader with no installer is refused for an install\n");
}

/* The other way round: a loader that publishes an installer has no
   profile to fetch, and the answer names the install that has it. */
static void
test_profile_of_a_loader_that_publishes_an_installer (void)
{
  Wait wait = { 0 };

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.watchdog = g_timeout_add_seconds (20, watchdog_fired, &wait);

  wait.operation = nuvasha_loaders_profile_async (nuvasha_LOADER_FORGE, "1.20.1",
                                              "1.20.1-47.2.0",
                                              on_profile_finished, &wait);

  g_main_loop_run (wait.loop);
  g_source_remove (wait.watchdog);
  g_main_loop_unref (wait.loop);

  g_assert_null (wait.profile);
  g_assert_error (wait.error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_UNSUPPORTED);
  g_assert_nonnull (strstr (wait.error->message, "nuvasha_loaders_install_async"));
  g_clear_error (&wait.error);

  g_print ("ok - a loader with no profile is refused for one\n");
}

int
main (void)
{
  g_print ("# Loader tests\n");

  test_supported_loaders ();
  test_fabric_version_list ();
  test_quilt_version_list ();
  test_empty_and_broken_answers ();
  test_which_loaders_publish_an_installer ();
  test_compatibility_filtering ();
  test_neoforge_version_list ();
  test_forge_version_list ();
  test_a_maven_that_answers_nothing ();
  test_versions_for_display ();
  test_fabric_profile ();
  test_quilt_profile ();
  test_profile_without_a_maven_address ();
  test_install_of_a_loader_that_publishes_none ();
  test_profile_of_a_loader_that_publishes_an_installer ();
  test_cancelled_versions_deliver_nothing ();

  return EXIT_SUCCESS;
}
