/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for aml-loaders.
 *
 * Every loader answers with a document of its own, and what AML has to
 * get right is reading it: which versions a loader has for one Minecraft
 * version, which of those it calls stable, and the document those
 * versions are, whose libraries publish a maven address instead of a
 * download.
 *
 * The answers come from the fixtures in tests/fixtures, trimmed from
 * what the loaders themselves published, so nothing here needs the
 * network. */

#include "aml-loaders.h"

#include "aml-minecraft-resolve.h"

#include <glib.h>
#include <stdlib.h>

#ifndef AML_TEST_FIXTURES
#define AML_TEST_FIXTURES "tests/fixtures"
#endif

static const AmlLoaderVersion *
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
  const AmlLoader *loaders = aml_loaders_supported ();
  guint count = aml_loaders_supported_count ();
  gboolean vanilla = FALSE;
  gboolean fabric = FALSE;

  /* Vanilla is what an instance is without a loader, so it is always in
     the list. */
  g_assert_cmpuint (count, >, 0);

  for (guint i = 0; i < count; i++)
    {
      g_assert_true (aml_loader_is_supported (loaders[i]));
      g_assert_nonnull (aml_loader_display_name (loaders[i]));

      if (loaders[i] == AML_LOADER_VANILLA)
        vanilla = TRUE;

      if (loaders[i] == AML_LOADER_FABRIC)
        fabric = TRUE;
    }

  g_assert_true (vanilla);
  g_assert_true (fabric);

  /* The names are what a user reads, never the string of the enum. */
  g_assert_cmpstr (aml_loader_display_name (AML_LOADER_VANILLA), ==, "Vanilla");
  g_assert_cmpstr (aml_loader_display_name (AML_LOADER_FABRIC), ==, "Fabric");
  g_assert_cmpstr (aml_loader_display_name (AML_LOADER_QUILT), ==, "Quilt");

  g_print ("ok - the loader lists hold the loaders AML installs\n");
}

/* ---------------------------------------------------------- *
 * What Fabric Meta answers
 * ---------------------------------------------------------- */

static void
test_fabric_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (AML_TEST_FIXTURES "/fabric-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = aml_loaders_versions_from_json (json, -1, &error);
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

  /* What the loader says about stability is what a list is filtered and
     labelled with. */
  g_assert_true (version_at (versions, 0)->stable);
  g_assert_false (version_at (versions, 1)->stable);
  g_assert_false (version_at (versions, 3)->stable);

  g_print ("ok - a Fabric list is read, newest first, with what is stable\n");
}

/* ---------------------------------------------------------- *
 * What Quilt Meta answers
 * ---------------------------------------------------------- */

static void
test_quilt_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (AML_TEST_FIXTURES "/quilt-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = aml_loaders_versions_from_json (json, -1, &error);
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

/* ---------------------------------------------------------- *
 * Answers that hold nothing
 * ---------------------------------------------------------- */

static void
test_empty_and_broken_answers (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  /* A loader with no version for that Minecraft version answers with an
     empty list, which is not the same as an error the user has to read
     twice. */
  versions = aml_loaders_versions_from_json ("[]", -1, &error);
  g_assert_null (versions);
  g_assert_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_VERSIONS);
  g_clear_error (&error);

  /* Something that is not a list at all is refused as well. */
  versions = aml_loaders_versions_from_json ("{\"error\": \"nope\"}", -1,
                                             &error);
  g_assert_null (versions);
  g_assert_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_VERSIONS);
  g_clear_error (&error);

  /* An entry that says nothing about a version is left out instead of
     becoming an empty row. */
  versions = aml_loaders_versions_from_json (
    "[{\"loader\": {}}, {\"loader\": {\"version\": \"0.19.5\"}}]", -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 1);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "0.19.5");

  g_print ("ok - an answer with nothing in it is refused, not shown\n");
}

/* ---------------------------------------------------------- *
 * The document of a loader version
 * ---------------------------------------------------------- */

static const AmlLibrary *
find_library (AmlVersionDescription *description, const char *name)
{
  for (guint i = 0; i < description->libraries->len; i++)
    {
      const AmlLibrary *library = g_ptr_array_index (description->libraries, i);

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
  g_autoptr (AmlVersionDescription) profile = NULL;
  g_autoptr (AmlVersionDescription) vanilla = NULL;
  g_autoptr (AmlVersionDescription) merged = NULL;
  const AmlLibrary *library = NULL;
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

  if (!g_file_get_contents (AML_TEST_FIXTURES "/fabric-profile.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  profile = aml_loaders_profile_from_json (json, -1, &error);
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
    const AmlArgument *argument = g_ptr_array_index (profile->jvm_arguments, 0);

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
  vanilla = aml_version_description_new_from_json (vanilla_document, -1,
                                                   &error);
  g_assert_no_error (error);
  g_assert_nonnull (vanilla);

  merged = aml_version_description_merge (vanilla, profile);
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
    const AmlLibrary *first = g_ptr_array_index (merged->libraries, 0);

    g_assert_cmpstr (first->name, ==, "org.lwjgl:lwjgl:3.3.3");
  }
  g_assert_cmpuint (merged->jvm_arguments->len, ==, 3);
  g_assert_cmpuint (merged->game_arguments->len, ==, 2);

  g_print ("ok - a Fabric profile becomes a document that merges over it\n");
}

static void
test_profile_without_a_maven_address (void)
{
  g_autoptr (GError) error = NULL;

  /* A library that names no jar at all is refused: a document that
     silently loses a library would start a game that cannot work. */
  g_assert_null (aml_loaders_profile_from_json (
    "{\"id\": \"x\", \"type\": \"release\","
    " \"libraries\": [{\"name\": \"no:where:1.0\"}]}", -1, &error));
  g_assert_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_MAVEN);
  g_clear_error (&error);

  /* Coordinates that make no path are refused as well, because the path
     they would write into the library directory has to be a name. */
  g_assert_null (aml_loaders_profile_from_json (
    "{\"id\": \"x\", \"type\": \"release\","
    " \"libraries\": [{\"name\": \"../evil:evil:1.0\","
    "                 \"url\": \"https://example.invalid/\"}]}", -1, &error));
  g_assert_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_MAVEN);

  g_print ("ok - a profile with no usable maven address is refused\n");
}

int
main (void)
{
  g_print ("# Loader tests\n");

  test_supported_loaders ();
  test_fabric_version_list ();
  test_quilt_version_list ();
  test_empty_and_broken_answers ();
  test_fabric_profile ();
  test_profile_without_a_maven_address ();

  return EXIT_SUCCESS;
}