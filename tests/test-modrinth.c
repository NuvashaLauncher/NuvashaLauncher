/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for aml-modrinth.
 *
 * What AML has to get right when it reads Modrinth is not the network
 * but the two things around it: the facets that narrow a search to the
 * instance it is for, and the rule that keeps an incompatible version
 * out of every list. Both are decided from data here, so nothing in
 * this file needs the network.
 *
 * The fixtures are trimmed from what the API answered; the two entries
 * that only exist to exercise a filter (a version for another Minecraft
 * version and one for another loader) are shaped after real ones, and
 * the fixture that carries all four dependency kinds says so inside. */

#include "aml-content-page.h"
#include "aml-modrinth.h"

#include <glib.h>

#ifndef AML_TEST_FIXTURES
#define AML_TEST_FIXTURES "tests/fixtures"
#endif

static char *
read_fixture (const char *name)
{
  g_autofree char *path = g_build_filename (AML_TEST_FIXTURES, name, NULL);
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;

  if (!g_file_get_contents (path, &json, NULL, &error))
    g_error ("the fixture \"%s\" cannot be read: %s", name, error->message);

  return g_steal_pointer (&json);
}

/* A search for @version and @loader, with the strings it owns. */
static AmlModrinthSearch *
search_new (AmlContentType type, const char *query, const char *version,
            const char *loader)
{
  AmlModrinthSearch *search = g_new0 (AmlModrinthSearch, 1);

  search->type = type;
  search->query = g_strdup (query != NULL ? query : "");
  search->minecraft_version = g_strdup (version);
  search->loader = loader != NULL ? g_strdup (loader) : NULL;

  return search;
}

/* ---------------------------------------------------------- *
 * The facets of a search
 * ---------------------------------------------------------- */

static void
test_facets_of_a_mod_search (void)
{
  g_autoptr (AmlModrinthSearch) search =
    search_new (AML_CONTENT_MOD, "", "1.20.1", "fabric");
  g_autofree char *facets = NULL;

  facets = aml_modrinth_facets (search);

  /* The project type, the Minecraft version and the loader, each one on
   * its own: that is what makes an answer compatible with the instance
   * instead of something to be refused later. */
  g_assert_cmpstr (
      facets, ==,
      "[[\"project_type:mod\"],[\"versions:1.20.1\"],[\"categories:fabric\"]]");

  g_print ("ok - a mod search is narrowed by version and by loader\n");
}

static void
test_facets_of_a_pack_search (void)
{
  g_autoptr (AmlModrinthSearch) search =
    search_new (AML_CONTENT_RESOURCE_PACK, NULL, "1.20.1", NULL);
  g_autofree char *facets = NULL;
  g_autofree char *shaders = NULL;

  search->categories = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (search->categories, g_strdup ("32x"));

  facets = aml_modrinth_facets (search);

  /* A resource pack is loaded by the game itself, so there is no loader
   * in the facets, only the Minecraft version and what was asked for. */
  g_assert_cmpstr (facets, ==,
                   "[[\"project_type:resourcepack\"],[\"versions:1.20.1\"],"
                   "[\"categories:32x\"]]");

  search->type = AML_CONTENT_SHADER_PACK;
  shaders = aml_modrinth_facets (search);
  g_assert_cmpstr (shaders, ==,
                   "[[\"project_type:shader\"],[\"versions:1.20.1\"],"
                   "[\"categories:32x\"]]");

  g_print ("ok - the other two are narrowed by version only\n");
}

static void
test_search_url_paginates (void)
{
  g_autoptr (AmlModrinthSearch) search =
    search_new (AML_CONTENT_MOD, "sodium", "1.20.1", "fabric");
  g_autofree char *url = NULL;
  g_autofree char *expected = NULL;

  search->offset = 40;
  search->limit = 20;

  url = aml_modrinth_search_url (search);
  expected = g_strdup_printf (
      AML_MODRINTH_URL "/search?query=sodium&facets=%s&offset=40&limit=20",
      "%5B%5B%22project_type%3Amod%22%5D%2C%5B%22versions%3A1.20.1%22%5D%2C"
      "%5B%22categories%3Afabric%22%5D%5D");

  g_assert_cmpstr (url, ==, expected);

  g_print ("ok - a search url carries the query, the facets and the page\n");
}

/* ---------------------------------------------------------- *
 * What a search answers
 * ---------------------------------------------------------- */

static void
test_search_results (void)
{
  g_autofree char *json = read_fixture ("modrinth-search-mods.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthResults) results = NULL;
  AmlModrinthProject *first = NULL;

  results = aml_modrinth_results_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->projects->len, ==, 3);

  /* What the pagination is read from. */
  g_assert_cmpuint (results->total_hits, ==, 17609);

  first = g_ptr_array_index (results->projects, 0);
  g_assert_cmpstr (first->project_id, ==, "AANobbMI");
  g_assert_cmpstr (first->slug, ==, "sodium");
  g_assert_cmpstr (first->title, ==, "Sodium");
  g_assert_cmpstr (first->author, ==, "jellysquid3");
  g_assert_cmpuint (first->downloads, ==, 237462072);
  g_assert_true (first->description[0] != '\0');
  g_assert_nonnull (first->icon_url);

  g_print ("ok - a search answer is read into rows and a total\n");
}

static void
test_search_without_hits (void)
{
  g_autofree char *json = read_fixture ("modrinth-search-empty.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthResults) results = NULL;

  /* Nothing found is an answer, not a failure. */
  results = aml_modrinth_results_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->projects->len, ==, 0);
  g_assert_cmpuint (results->total_hits, ==, 0);

  g_print ("ok - a search that found nothing is an empty list\n");
}

/* ---------------------------------------------------------- *
 * A project in full
 * ---------------------------------------------------------- */

static void
test_project_details (void)
{
  g_autofree char *json = read_fixture ("modrinth-project.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthDetails) details = NULL;

  details = aml_modrinth_details_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (details);

  g_assert_cmpstr (details->project_id, ==, "AANobbMI");
  g_assert_cmpstr (details->title, ==, "Sodium");
  g_assert_cmpstr (details->license, ==, "LicenseRef-Polyform-Shield-1.0.0");
  g_assert_cmpuint (details->downloads, ==, 237896647);
  g_assert_cmpuint (details->followers, ==, 41479);

  /* The license is an object with an id in the answer. */
  g_assert_nonnull (details->source_url);

  /* A project answer names the team rather than an author, and a row
   * shows one name, so the detail page has to have one too. */
  g_assert_nonnull (details->author);

  g_print ("ok - a project answer is read, license and author included\n");
}

/* ---------------------------------------------------------- *
 * Which versions fit
 * ---------------------------------------------------------- */

static void
test_versions_of_a_mod (void)
{
  g_autofree char *json = read_fixture ("modrinth-versions-mods.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  AmlModrinthVersion *version = NULL;

  versions = aml_modrinth_versions_from_json (
      json, -1, AML_CONTENT_MOD, "1.20.1", "fabric", &error);
  g_assert_no_error (error);
  g_assert_nonnull (versions);

  /* The fixture holds five versions for 1.20(.1): one for Fabric, one
   * for Forge, one for NeoForge, one older for 1.19.4 and one for 1.21.
   * Only the two that run on Fabric 1.20.1 may be listed. */
  g_assert_cmpuint (versions->len, ==, 2);

  /* Newest first, which is the order a user reads them in. */
  version = g_ptr_array_index (versions, 0);
  g_assert_cmpstr (version->version_number, ==, "11.1.136+fabric");
  g_assert_cmpstr (version->name, ==, "[Fabric 1.20(.1)] v11.1.136");

  version = g_ptr_array_index (versions, 1);
  g_assert_cmpstr (version->version_number, ==, "11.1.118+fabric");

  /* The file of a version is what gets installed, with its hash, so that
   * the download can be verified against what Modrinth published. */
  {
    AmlModrinthFile *file = aml_modrinth_version_primary_file (version);

    g_assert_nonnull (file);
    g_assert_cmpstr (file->filename, ==, "cloth-config-11.1.118-fabric.jar");
    g_assert_cmpstr (file->sha1, ==,
                     "f669f899d707b9e9acf1c239b0ae6b2c8be95361");
    g_assert_true (file->primary);
  }

  g_print ("ok - a mod is only listed for its version and its loader\n");
}

static void
test_versions_of_a_pack (void)
{
  g_autofree char *json =
    read_fixture ("modrinth-versions-resource-pack.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  AmlModrinthVersion *version = NULL;

  /* No loader is asked for: the game loads a pack itself. */
  versions = aml_modrinth_versions_from_json (
      json, -1, AML_CONTENT_RESOURCE_PACK, "1.20.1", NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 1);

  version = g_ptr_array_index (versions, 0);
  g_assert_cmpstr (version->version_number, ==, "1.20.1-june-2026");

  /* The same answer asked for another loader is not narrowed: a pack
   * that says "minecraft" runs whatever runs the game. */
  {
    g_autoptr (GPtrArray) any = aml_modrinth_versions_from_json (
        json, -1, AML_CONTENT_RESOURCE_PACK, "1.20", "forge", &error);

    g_assert_no_error (error);
    g_assert_cmpuint (any->len, ==, 2);
  }

  g_print ("ok - a pack is only listed for its Minecraft version\n");
}

static void
test_no_compatible_version (void)
{
  g_autofree char *json = read_fixture ("modrinth-versions-mods.json");
  g_autoptr (GError) error = NULL;
  GPtrArray *versions = NULL;
  g_autofree char *message = NULL;
  g_autofree char *shaders = NULL;

  /* Nothing in the fixture runs on Minecraft 1.7.10, and nothing is
   * listed rather than something listed and refused afterwards. */
  versions = aml_modrinth_versions_from_json (
      json, -1, AML_CONTENT_MOD, "1.7.10", "fabric", &error);
  g_assert_null (versions);
  g_assert_error (error, AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_NOT_FOUND);

  /* The words a user reads are the ones the page shows. */
  g_assert_cmpstr (error->message, ==,
                   "No compatible version found for Minecraft 1.7.10 and "
                   "fabric.");

  message = aml_modrinth_no_compatible_message (AML_CONTENT_MOD, "1.7.10",
                                                "quilt");
  g_assert_cmpstr (message, ==,
                   "No compatible version found for Minecraft 1.7.10 and "
                   "quilt.");

  shaders = aml_modrinth_no_compatible_message (AML_CONTENT_SHADER_PACK,
                                                 "1.7.10", NULL);
  g_assert_cmpstr (shaders, ==,
                   "No compatible version found for Minecraft 1.7.10.");

  g_print ("ok - nothing compatible is said in words, not left to be "
           "refused\n");
}

/* ---------------------------------------------------------- *
 * A file on disk
 * ---------------------------------------------------------- */

static void
test_version_by_hash (void)
{
  g_autofree char *json = read_fixture ("modrinth-version-by-hash.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthVersion) version = NULL;
  AmlModrinthFile *file = NULL;

  version = aml_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (version);

  g_assert_cmpstr (version->version_id, ==, "mhZtY2lR");
  g_assert_cmpstr (version->project_id, ==, "AANobbMI");
  g_assert_cmpstr (version->name, ==, "Sodium 0.5.8");
  g_assert_cmpstr (version->version_number, ==, "mc1.20.1-0.5.8");

  /* What an installed file shows under its name. */
  file = aml_modrinth_version_primary_file (version);
  g_assert_nonnull (file);
  g_assert_cmpstr (file->sha1, ==,
                   "bdd6b8ac84b46ae56a3bbb761ce430c48e42e00d");
  g_assert_cmpint (file->size, ==, 942134);

  /* And it is a version this instance could run. */
  g_assert_true (aml_modrinth_version_fits (version, AML_CONTENT_MOD, "1.20.1",
                                           "fabric"));
  g_assert_false (
      aml_modrinth_version_fits (version, AML_CONTENT_MOD, "1.21", "fabric"));

  g_print ("ok - the hash of a file says which version it is\n");
}

static void
test_update_by_hash (void)
{
  g_autofree char *json = read_fixture ("modrinth-update-by-hash.json");
  g_autofree char *installed = read_fixture ("modrinth-version-by-hash.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthVersion) installed_version = NULL;
  g_autoptr (AmlModrinthVersion) update = NULL;
  AmlModrinthFile *file = NULL;

  installed_version = aml_modrinth_version_from_json (installed, -1, &error);
  g_assert_no_error (error);

  update = aml_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (update);

  /* The update endpoint answers with the newer version of the same
   * project, so the update is only what the page offers when it is
   * another version of what is installed. */
  g_assert_cmpstr (update->project_id, ==, installed_version->project_id);
  g_assert_cmpstr (update->version_id, ==, "OihdIimA");
  g_assert_true (strcmp (update->version_id, installed_version->version_id) != 0);
  g_assert_true (
      aml_modrinth_version_fits (update, AML_CONTENT_MOD, "1.20.1", "fabric"));

  file = aml_modrinth_version_primary_file (update);
  g_assert_cmpstr (file->filename, ==, "sodium-fabric-0.5.13+mc1.20.1.jar");
  g_assert_cmpstr (file->sha1, ==,
                   "bcdbf37d9494e405cba210d7a80ed58e756297b5");

  g_print ("ok - the hash update answer is another version of the same "
           "project\n");
}

/* ---------------------------------------------------------- *
 * What an install needs
 * ---------------------------------------------------------- */

static void
test_dependencies_of_a_version (void)
{
  g_autofree char *json =
    read_fixture ("modrinth-version-dependencies.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthVersion) version = NULL;
  g_autoptr (GPtrArray) pending = NULL;
  g_autoptr (GPtrArray) conflicts = NULL;

  version = aml_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (version->dependencies->len, ==, 1);

  pending = aml_modrinth_resolve_install (version, NULL, &conflicts, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (pending->len, ==, 1);
  g_assert_cmpuint (conflicts->len, ==, 0);

  {
    AmlModrinthDependency *dependency = g_ptr_array_index (pending, 0);

    g_assert_cmpint (dependency->type, ==, AML_DEPENDENCY_REQUIRED);
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
    again = aml_modrinth_resolve_install (version, installed, NULL, &error);

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
  g_autoptr (AmlModrinthVersion) version = NULL;
  g_autoptr (GPtrArray) pending = NULL;
  g_autoptr (GPtrArray) conflicts = NULL;
  g_autoptr (GHashTable) installed =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  version = aml_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (version->dependencies->len, ==, 5);

  /* Embeddium's conflicting project is in the instance, so the install
   * has to say so before anything is written. */
  g_hash_table_add (installed, g_strdup ("4ZqxOvjD"));

  pending = aml_modrinth_resolve_install (version, installed, &conflicts,
                                          &error);
  g_assert_no_error (error);

  /* Only what the version cannot run without: the optional and the
   * embedded ones are not fetched, and the duplicate of the required
   * dependency is one file, not two. */
  g_assert_cmpuint (pending->len, ==, 1);
  g_assert_cmpstr (((AmlModrinthDependency *) g_ptr_array_index (pending, 0))
                       ->project_id,
                   ==, "AANobbMI");

  g_assert_cmpuint (conflicts->len, ==, 1);
  g_assert_cmpstr (((AmlModrinthDependency *) g_ptr_array_index (conflicts, 0))
                       ->project_id,
                   ==, "4ZqxOvjD");
  g_assert_cmpstr (aml_modrinth_dependency_to_string (
                       ((AmlModrinthDependency *)
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
  g_autoptr (AmlModrinthVersion) version = NULL;
  GPtrArray *pending = NULL;

  version = aml_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);

  /* A dependency that names no project names nothing that could be
   * fetched, and the install says so instead of going on without it. */
  pending = aml_modrinth_resolve_install (version, NULL, NULL, &error);
  g_assert_null (pending);
  g_assert_error (error, AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_PARSE);

  g_print ("ok - a dependency that cannot be fetched stops the install\n");
}

/* ---------------------------------------------------------- *
 * The files of an instance
 * ---------------------------------------------------------- */

static void
test_folder_and_disabled_names (void)
{
  g_autofree char *mods = NULL;
  g_autofree char *packs = NULL;
  g_autofree char *disabled = NULL;
  g_autofree char *enabled = NULL;
  g_autofree char *again = NULL;

  mods = aml_content_folder_path ("/tmp/instance", AML_CONTENT_MOD);
  packs = aml_content_folder_path ("/tmp/instance",
                                   AML_CONTENT_RESOURCE_PACK);
  g_assert_cmpstr (mods, ==, "/tmp/instance/mods");
  g_assert_cmpstr (packs, ==, "/tmp/instance/resourcepacks");

  /* Disabling puts the suffix on the name, which is what the loaders and
   * the game stop loading; enabling takes it off again. */
  disabled = aml_content_disabled_name ("sodium.jar", FALSE);
  g_assert_cmpstr (disabled, ==, "sodium.jar.disabled");

  enabled = aml_content_disabled_name (disabled, TRUE);
  g_assert_cmpstr (enabled, ==, "sodium.jar");

  /* A name that carries it twice does not get it twice. */
  again = aml_content_disabled_name (disabled, FALSE);
  g_assert_cmpstr (again, ==, "sodium.jar.disabled");

  g_print ("ok - a disabled file is the same file with a suffix on\n");
}

/* What the last scan of the test answered, and the loop that waits for
 * it. */
static GPtrArray *scanned = NULL;
static GMainLoop *scanning = NULL;

static void
on_scan_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data G_GNUC_UNUSED)
{
  g_clear_pointer (&scanned, g_ptr_array_unref);
  scanned = aml_content_scan_finish (result, NULL);
  g_main_loop_quit (scanning);
}

/* Reads a content folder and waits for it, which is the asynchronous
 * way the page does the same. (transfer full) */
static GPtrArray *
scan (const char *instance, AmlContentType type)
{
  GPtrArray *files = NULL;

  scanned = NULL;
  scanning = g_main_loop_new (NULL, FALSE);
  aml_content_scan_async (instance, type, NULL, on_scan_finished, NULL);
  g_main_loop_run (scanning);

  /* GMainLoop is refcounted but is not a GObject, so it cannot be given
   * to g_clear_object(). */
  g_main_loop_unref (scanning);
  scanning = NULL;

  files = scanned;
  scanned = NULL;

  return files;
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

  instance = g_dir_make_tmp ("aml-content-XXXXXX", NULL);
  g_assert_nonnull (instance);

  folder_path = aml_content_folder_path (instance, AML_CONTENT_MOD);
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

  lithium = scan (instance, AML_CONTENT_MOD);

  /* Only the mods are listed, and both are: a disabled one is still in
     the folder and the user has to see it to enable it again. */
  g_assert_nonnull (lithium);
  g_assert_cmpuint (lithium->len, ==, 2);

  {
    AmlContentFile *disabled = g_ptr_array_index (lithium, 0);
    AmlContentFile *enabled = g_ptr_array_index (lithium, 1);

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
      ((AmlContentFile *) g_ptr_array_index (lithium, 0))->path;
    g_autofree char *enabled_path =
      g_build_filename (folder_path, "lithium.jar", NULL);

    /* A disabled file is loaded again by taking the suffix off. */
    g_free (back);
    back = aml_content_set_disabled (disabled_path, TRUE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (back, ==, enabled_path);
    g_assert_true (g_file_test (back, G_FILE_TEST_EXISTS));

    /* Asking for the state a file is already in is not a rename. */
    same = aml_content_set_disabled (back, TRUE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (same, ==, back);
    g_assert_true (g_file_test (same, G_FILE_TEST_EXISTS));

    /* And it goes back to being a file the loader skips. */
    renamed = aml_content_set_disabled (back, FALSE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (renamed, ==, disabled_path);
    g_assert_true (g_file_test (renamed, G_FILE_TEST_EXISTS));
  }

  /* The folder is read again with the file disabled under its new name,
     and the resource pack folder of an instance that has none is
     simply empty. */
  {
    g_autoptr (GPtrArray) again = scan (instance, AML_CONTENT_MOD);

    g_assert_cmpuint (again->len, ==, 2);
    g_assert_false (((AmlContentFile *) g_ptr_array_index (again, 0))->enabled);
  }

  {
    g_autoptr (GPtrArray) none = scan (instance, AML_CONTENT_SHADER_PACK);

    /* A folder that does not exist holds no files, which is what an
       instance that never installed a shader pack looks like. */
    g_assert_nonnull (none);
    g_assert_cmpuint (none->len, ==, 0);
  }

  g_print ("ok - the folder is read, and enable and disable rename it\n");
}

/* ---------------------------------------------------------- *
 * The content kinds
 * ---------------------------------------------------------- */

static void
test_content_types (void)
{
  g_assert_cmpstr (aml_content_type_to_string (AML_CONTENT_MOD), ==, "mod");
  g_assert_cmpstr (aml_content_type_to_string (AML_CONTENT_RESOURCE_PACK), ==,
                   "resourcepack");
  g_assert_cmpstr (aml_content_type_to_string (AML_CONTENT_SHADER_PACK), ==,
                   "shader");

  g_assert_cmpstr (aml_content_type_folder (AML_CONTENT_MOD), ==, "mods");
  g_assert_cmpstr (aml_content_type_folder (AML_CONTENT_RESOURCE_PACK), ==,
                   "resourcepacks");
  g_assert_cmpstr (aml_content_type_folder (AML_CONTENT_SHADER_PACK), ==,
                   "shaderpacks");

  g_assert_cmpstr (aml_content_type_extension (AML_CONTENT_MOD), ==, ".jar");
  g_assert_cmpstr (aml_content_type_extension (AML_CONTENT_RESOURCE_PACK), ==,
                   ".zip");

  /* Only a mod needs a loader, which is what makes the three pages the
   * same page with a different content type. */
  g_assert_true (aml_content_type_needs_loader (AML_CONTENT_MOD));
  g_assert_false (aml_content_type_needs_loader (AML_CONTENT_RESOURCE_PACK));
  g_assert_false (aml_content_type_needs_loader (AML_CONTENT_SHADER_PACK));

  g_print ("ok - the three kinds name their own folders and loaders\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/modrinth/facets-mod", test_facets_of_a_mod_search);
  g_test_add_func ("/modrinth/facets-pack", test_facets_of_a_pack_search);
  g_test_add_func ("/modrinth/search-url", test_search_url_paginates);
  g_test_add_func ("/modrinth/search-results", test_search_results);
  g_test_add_func ("/modrinth/search-empty", test_search_without_hits);
  g_test_add_func ("/modrinth/project", test_project_details);
  g_test_add_func ("/modrinth/versions-mod", test_versions_of_a_mod);
  g_test_add_func ("/modrinth/versions-pack", test_versions_of_a_pack);
  g_test_add_func ("/modrinth/no-compatible", test_no_compatible_version);
  g_test_add_func ("/modrinth/version-by-hash", test_version_by_hash);
  g_test_add_func ("/modrinth/update-by-hash", test_update_by_hash);
  g_test_add_func ("/modrinth/dependencies", test_dependencies_of_a_version);
  g_test_add_func ("/modrinth/dependency-kinds", test_dependency_kinds);
  g_test_add_func ("/modrinth/dependency-broken",
                   test_dependency_without_project);
  g_test_add_func ("/modrinth/content-types", test_content_types);
  g_test_add_func ("/modrinth/folder", test_folder_and_disabled_names);
  g_test_add_func ("/modrinth/scan", test_scan_and_toggle);

  return g_test_run ();
}