#include "test-modrinth-common.h"

static void
test_facets_of_a_mod_search (void)
{
  g_autoptr (nuvashalauncherModrinthSearch) search =
    search_new (nuvashalauncher_CONTENT_MOD, "", "1.20.1", "fabric");
  g_autofree char *facets = NULL;

  facets = nuvashalauncher_modrinth_facets (search);

  /* The project type, the Minecraft version and the loader, each one on
   * its own: that is what makes an answer compatible with the instance
   * instead of something to be refused later. */
  g_assert_cmpstr (
      facets, ==,
      "[[\"project_type:mod\"],[\"versions:1.20.1\"],[\"categories:fabric\"]]");

  g_print ("ok - a mod search is narrowed by version and by loader\n");
}

static void
test_facets_of_a_quilt_search (void)
{
  g_autoptr (nuvashalauncherModrinthSearch) search =
    search_new (nuvashalauncher_CONTENT_MOD, "", "1.20.1", "quilt");
  g_autofree char *facets = nuvashalauncher_modrinth_facets (search);

  g_assert_cmpstr (
      facets, ==,
      "[[\"project_type:mod\"],[\"versions:1.20.1\"],"
      "[\"categories:quilt\",\"categories:fabric\"]]");

  g_print ("ok - a quilt search asks for quilt and fabric mods\n");
}

static void
test_facets_of_a_pack_search (void)
{
  g_autoptr (nuvashalauncherModrinthSearch) search =
    search_new (nuvashalauncher_CONTENT_RESOURCE_PACK, NULL, "1.20.1", NULL);
  g_autofree char *facets = NULL;
  g_autofree char *shaders = NULL;

  search->categories = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (search->categories, g_strdup ("32x"));

  facets = nuvashalauncher_modrinth_facets (search);

  /* A resource pack is loaded by the game itself, so there is no loader
   * in the facets, only the Minecraft version and what was asked for. */
  g_assert_cmpstr (facets, ==,
                   "[[\"project_type:resourcepack\"],[\"versions:1.20.1\"],"
                   "[\"categories:32x\"]]");

  search->type = nuvashalauncher_CONTENT_SHADER_PACK;
  shaders = nuvashalauncher_modrinth_facets (search);
  g_assert_cmpstr (shaders, ==,
                   "[[\"project_type:shader\"],[\"versions:1.20.1\"],"
                   "[\"categories:32x\"]]");

  g_print ("ok - the other two are narrowed by version only\n");
}

static void
test_categories_filter_by_project_type (void)
{
  g_autofree char *json = read_fixture ("modrinth-categories.json");
  g_autoptr (GPtrArray) shaders = NULL;
  g_autoptr (GPtrArray) modpacks = NULL;
  g_autoptr (GPtrArray) resourcepacks = NULL;
  nuvashalauncherModrinthCategory *category = NULL;

  shaders = nuvashalauncher_modrinth_categories_from_json (json, -1, "shader", NULL);
  g_assert_nonnull (shaders);
  g_assert_cmpuint (shaders->len, ==, 2);

  category = g_ptr_array_index (shaders, 0);
  g_assert_cmpstr (category->name, ==, "bloom");
  g_assert_cmpstr (category->header, ==, "features");

  category = g_ptr_array_index (shaders, 1);
  g_assert_cmpstr (category->name, ==, "colored-lighting");

  modpacks = nuvashalauncher_modrinth_categories_from_json (json, -1, "modpack", NULL);
  g_assert_cmpuint (modpacks->len, ==, 1);
  g_assert_cmpstr (
      ((nuvashalauncherModrinthCategory *) g_ptr_array_index (modpacks, 0))->name, ==,
      "kitchen-sink");

  resourcepacks =
      nuvashalauncher_modrinth_categories_from_json (json, -1, "resourcepack", NULL);
  g_assert_cmpuint (resourcepacks->len, ==, 1);

  g_print ("ok - categories are filtered by project type\n");
}

static void
test_category_labels (void)
{
  g_autofree char *one = nuvashalauncher_modrinth_category_label ("colored-lighting");
  g_autofree char *two = nuvashalauncher_modrinth_category_label ("512x+");
  g_autofree char *three = nuvashalauncher_modrinth_category_label ("kitchen-sink");
  g_autofree char *resolution = nuvashalauncher_modrinth_category_label ("8x-");
  g_autofree char *acronym = nuvashalauncher_modrinth_category_label ("pbr");
  g_autofree char *heading = nuvashalauncher_modrinth_category_label ("performance impact");
  g_autofree char *worldgen = nuvashalauncher_modrinth_category_label ("worldgen");

  g_assert_cmpstr (one, ==, "Colored Lighting");
  g_assert_cmpstr (two, ==, "512x+");
  g_assert_cmpstr (three, ==, "Kitchen Sink");
  g_assert_cmpstr (resolution, ==, "8x-");
  g_assert_cmpstr (acronym, ==, "PBR");
  g_assert_cmpstr (heading, ==, "Performance Impact");
  g_assert_cmpstr (worldgen, ==, "World Generation");

  g_print ("ok - category labels are readable\n");
}

static void
test_facets_with_categories (void)
{
  g_autoptr (nuvashalauncherModrinthSearch) search =
    search_new (nuvashalauncher_CONTENT_SHADER_PACK, "", "1.20.1", NULL);
  g_autofree char *facets = NULL;

  search->categories = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (search->categories, g_strdup ("colored-lighting"));

  facets = nuvashalauncher_modrinth_facets (search);

  g_assert_cmpstr (facets, ==,
                   "[[\"project_type:shader\"],[\"versions:1.20.1\"],"
                   "[\"categories:colored-lighting\"]]");

  g_print ("ok - selected categories build facets\n");
}

static void
test_search_url_paginates (void)
{
  g_autoptr (nuvashalauncherModrinthSearch) search =
    search_new (nuvashalauncher_CONTENT_MOD, "sodium", "1.20.1", "fabric");
  g_autofree char *url = NULL;
  g_autofree char *expected = NULL;

  search->offset = 40;
  search->limit = 20;

  url = nuvashalauncher_modrinth_search_url (search);
  expected = g_strdup_printf (
      nuvashalauncher_MODRINTH_URL "/search?query=sodium&facets=%s&offset=40&limit=20",
      "%5B%5B%22project_type%3Amod%22%5D%2C%5B%22versions%3A1.20.1%22%5D%2C"
      "%5B%22categories%3Afabric%22%5D%5D");

  g_assert_cmpstr (url, ==, expected);

  g_print ("ok - a search url carries the query, the facets and the page\n");
}

static void
test_modpack_filters_are_or_groups_and_sort (void)
{
  g_autoptr (nuvashalauncherModrinthSearch) search =
    search_new (nuvashalauncher_CONTENT_MODPACK, "pack", NULL, NULL);
  g_autofree char *facets = NULL;
  g_autofree char *url = NULL;

  search->minecraft_versions = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (search->minecraft_versions, g_strdup ("1.20.1"));
  g_ptr_array_add (search->minecraft_versions, g_strdup ("1.21.1"));
  search->loaders = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (search->loaders, g_strdup ("fabric"));
  g_ptr_array_add (search->loaders, g_strdup ("neoforge"));
  search->sort = g_strdup ("updated");
  search->offset = 40;
  search->limit = 20;

  facets = nuvashalauncher_modrinth_facets (search);
  g_assert_cmpstr (facets, ==,
    "[[\"project_type:modpack\"],[\"versions:1.20.1\",\"versions:1.21.1\"],"
    "[\"categories:fabric\",\"categories:neoforge\"]]");
  url = nuvashalauncher_modrinth_search_url (search);
  g_assert_nonnull (strstr (url, "index=updated"));
  g_assert_nonnull (strstr (url, "offset=40"));
  g_print ("ok - modpack versions and loaders preserve facet grouping and sorting\n");
}

static void
test_search_results (void)
{
  g_autofree char *json = read_fixture ("modrinth-search-mods.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModrinthResults) results = NULL;
  nuvashalauncherModrinthProject *first = NULL;

  results = nuvashalauncher_modrinth_results_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->projects->len, ==, 3);

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
  g_autoptr (nuvashalauncherModrinthResults) results = NULL;

  results = nuvashalauncher_modrinth_results_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->projects->len, ==, 0);
  g_assert_cmpuint (results->total_hits, ==, 0);

  g_print ("ok - a search that found nothing is an empty list\n");
}

static void
test_search_rejects_malformed_hit (void)
{
  const char *json =
    "{\"total_hits\":1,\"hits\":[{\"slug\":\"missing-id\"}]}";
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModrinthResults) results =
    nuvashalauncher_modrinth_results_from_json (json, -1, &error);

  g_assert_null (results);
  g_assert_error (error, nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_PARSE);
}

static void
test_project_details (void)
{
  g_autofree char *json = read_fixture ("modrinth-project.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModrinthDetails) details = NULL;

  details = nuvashalauncher_modrinth_details_from_json (json, -1, &error);
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

static void
test_versions_of_a_mod (void)
{
  g_autofree char *json = read_fixture ("modrinth-versions-mods.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  nuvashalauncherModrinthVersion *version = NULL;

  versions = nuvashalauncher_modrinth_versions_from_json (
      json, -1, nuvashalauncher_CONTENT_MOD, "1.20.1", "fabric", &error);
  g_assert_no_error (error);
  g_assert_nonnull (versions);

  /* The fixture holds five versions for 1.20(.1): one for Fabric, one
   * for Forge, one for NeoForge, one older for 1.19.4 and one for 1.21.
   * Only the two that run on Fabric 1.20.1 may be listed. */
  g_assert_cmpuint (versions->len, ==, 2);

  version = g_ptr_array_index (versions, 0);
  g_assert_cmpstr (version->version_number, ==, "11.1.136+fabric");
  g_assert_cmpstr (version->name, ==, "[Fabric 1.20(.1)] v11.1.136");

  version = g_ptr_array_index (versions, 1);
  g_assert_cmpstr (version->version_number, ==, "11.1.118+fabric");

  /* The file of a version is what gets installed, with its hash, so that
   * the download can be verified against what Modrinth published. */
  {
    nuvashalauncherModrinthFile *file = nuvashalauncher_modrinth_version_primary_file (version);

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
  nuvashalauncherModrinthVersion *version = NULL;

  /* No loader is asked for: the game loads a pack itself. */
  versions = nuvashalauncher_modrinth_versions_from_json (
      json, -1, nuvashalauncher_CONTENT_RESOURCE_PACK, "1.20.1", NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 1);

  version = g_ptr_array_index (versions, 0);
  g_assert_cmpstr (version->version_number, ==, "1.20.1-june-2026");

  /* The same answer asked for another loader is not narrowed: a pack
   * that says "minecraft" runs whatever runs the game. */
  {
    g_autoptr (GPtrArray) any = nuvashalauncher_modrinth_versions_from_json (
        json, -1, nuvashalauncher_CONTENT_RESOURCE_PACK, "1.20", "forge", &error);

    g_assert_no_error (error);
    g_assert_cmpuint (any->len, ==, 2);
  }

  g_print ("ok - a pack is only listed for its Minecraft version\n");
}

static void
test_versions_of_a_shader (void)
{
  g_autofree char *json =
    read_fixture ("modrinth-shader-versions.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  nuvashalauncherModrinthVersion *version = NULL;

  /* A loader is not asked for, and not asked about: the loaders a shader
     publishes are the programs that can read it, not the one running
     beside the game. */
  versions = nuvashalauncher_modrinth_versions_from_json (
      json, -1, nuvashalauncher_CONTENT_SHADER_PACK, "1.20.1", "fabric", &error);
  g_assert_no_error (error);
  g_assert_nonnull (versions);

  /* Only the two that list 1.20.1. The third does not, and is left out
     here rather than shown and refused afterwards. */
  g_assert_cmpuint (versions->len, ==, 2);

  version = g_ptr_array_index (versions, 0);
  g_assert_cmpstr (version->version_number, ==, "r5.9.3");

  /* The loaders it does publish are kept, not overwritten: a details
     page shows them and they are the reason the pack is usable at all. */
  g_assert_cmpuint (version->loaders->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (version->loaders, 0), ==, "iris");
  g_assert_cmpstr (g_ptr_array_index (version->loaders, 1), ==, "optifine");

  /* The rule itself, on the fixture's own objects: a shader fits its
     game version whatever loaders it names, and does not fit a game
     version it does not list. */
  {
    g_autoptr (nuvashalauncherModrinthVersion) fitted =
      nuvashalauncher_modrinth_version_copy (g_ptr_array_index (versions, 0));
    g_autoptr (nuvashalauncherModrinthVersion) other_game =
      version_from_fixture (json, "r2.1");

    g_assert_nonnull (fitted);
    g_assert_nonnull (other_game);

    /* Whatever runs beside the game, the shader fits its version. */
    g_assert_true (nuvashalauncher_modrinth_version_fits (fitted, nuvashalauncher_CONTENT_SHADER_PACK,
                                             "1.20.1", "fabric"));
    g_assert_true (nuvashalauncher_modrinth_version_fits (fitted, nuvashalauncher_CONTENT_SHADER_PACK,
                                             "1.20.1", "iris"));
    g_assert_true (nuvashalauncher_modrinth_version_fits (fitted, nuvashalauncher_CONTENT_SHADER_PACK,
                                             "1.20.1", "optifine"));

    /* A game version it does not list is refused, and a mod is still
       narrowed by its loader: the rule is not weakened to make shaders
       work. */
    /* r5.9.3 lists 76 game versions and r2.1 lists neither 1.20.1 nor
       1.21, so each is refused a version it does not name. */
    g_assert_false (nuvashalauncher_modrinth_version_fits (fitted,
                                              nuvashalauncher_CONTENT_SHADER_PACK,
                                              "1.4.7", "iris"));
    g_assert_false (nuvashalauncher_modrinth_version_fits (other_game,
                                              nuvashalauncher_CONTENT_SHADER_PACK,
                                              "1.21", "iris"));
    g_assert_false (nuvashalauncher_modrinth_version_fits (other_game,
                                              nuvashalauncher_CONTENT_SHADER_PACK,
                                              "1.20.1", "iris"));
    g_assert_false (nuvashalauncher_modrinth_version_fits (fitted, nuvashalauncher_CONTENT_MOD,
                                               "1.20.1", "fabric"));
  }

  g_print ("ok - a shader is only listed for its Minecraft version\n");
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
  versions = nuvashalauncher_modrinth_versions_from_json (
      json, -1, nuvashalauncher_CONTENT_MOD, "1.7.10", "fabric", &error);
  g_assert_null (versions);
  g_assert_error (error, nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_NOT_FOUND);

  /* The words a user reads are the ones the page shows. */
  g_assert_cmpstr (error->message, ==,
                   "No compatible version found for Minecraft 1.7.10 and "
                   "fabric.");

  message = nuvashalauncher_modrinth_no_compatible_message (nuvashalauncher_CONTENT_MOD, "1.7.10",
                                                "quilt");
  g_assert_cmpstr (message, ==,
                   "No compatible version found for Minecraft 1.7.10 and "
                   "quilt.");

  shaders = nuvashalauncher_modrinth_no_compatible_message (nuvashalauncher_CONTENT_SHADER_PACK,
                                                 "1.7.10", NULL);
  g_assert_cmpstr (shaders, ==,
                   "No compatible version found for Minecraft 1.7.10.");

  g_print ("ok - nothing compatible is said in words, not left to be "
           "refused\n");
}

static void
test_version_by_hash (void)
{
  g_autofree char *json = read_fixture ("modrinth-version-by-hash.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModrinthVersion) version = NULL;
  nuvashalauncherModrinthFile *file = NULL;

  version = nuvashalauncher_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (version);

  g_assert_cmpstr (version->version_id, ==, "mhZtY2lR");
  g_assert_cmpstr (version->project_id, ==, "AANobbMI");
  g_assert_cmpstr (version->name, ==, "Sodium 0.5.8");
  g_assert_cmpstr (version->version_number, ==, "mc1.20.1-0.5.8");

  /* What an installed file shows under its name. */
  file = nuvashalauncher_modrinth_version_primary_file (version);
  g_assert_nonnull (file);
  g_assert_cmpstr (file->sha1, ==,
                   "bdd6b8ac84b46ae56a3bbb761ce430c48e42e00d");
  g_assert_cmpint (file->size, ==, 942134);

  /* And it is a version this instance could run. */
  g_assert_true (nuvashalauncher_modrinth_version_fits (version, nuvashalauncher_CONTENT_MOD, "1.20.1",
                                           "fabric"));
  g_assert_false (
      nuvashalauncher_modrinth_version_fits (version, nuvashalauncher_CONTENT_MOD, "1.21", "fabric"));

  /* Quilt runs Fabric mods, so a version that names only Fabric is a
     version of a Quilt instance too, and nothing else gets it. */
  g_clear_pointer (&version->loaders, g_ptr_array_unref);
  version->loaders = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (version->loaders, g_strdup ("fabric"));

  g_assert_true (nuvashalauncher_modrinth_version_fits (version, nuvashalauncher_CONTENT_MOD, "1.20.1",
                                            "quilt"));
  g_assert_false (nuvashalauncher_modrinth_version_fits (version, nuvashalauncher_CONTENT_MOD,
                                             "1.20.1", "neoforge"));

  g_print ("ok - the hash of a file says which version it is\n");
}

static void
test_update_by_hash (void)
{
  g_autofree char *json = read_fixture ("modrinth-update-by-hash.json");
  g_autofree char *installed = read_fixture ("modrinth-version-by-hash.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModrinthVersion) installed_version = NULL;
  g_autoptr (nuvashalauncherModrinthVersion) update = NULL;
  nuvashalauncherModrinthFile *file = NULL;

  installed_version = nuvashalauncher_modrinth_version_from_json (installed, -1, &error);
  g_assert_no_error (error);

  update = nuvashalauncher_modrinth_version_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (update);

  /* The update endpoint answers with the newer version of the same
   * project, so the update is only what the page offers when it is
   * another version of what is installed. */
  g_assert_cmpstr (update->project_id, ==, installed_version->project_id);
  g_assert_cmpstr (update->version_id, ==, "OihdIimA");
  g_assert_true (strcmp (update->version_id, installed_version->version_id) != 0);
  g_assert_true (
      nuvashalauncher_modrinth_version_fits (update, nuvashalauncher_CONTENT_MOD, "1.20.1", "fabric"));

  file = nuvashalauncher_modrinth_version_primary_file (update);
  g_assert_cmpstr (file->filename, ==, "sodium-fabric-0.5.13+mc1.20.1.jar");
  g_assert_cmpstr (file->sha1, ==,
                   "bcdbf37d9494e405cba210d7a80ed58e756297b5");

  /* The endpoint can also answer with the version already installed, and
     the page must not offer that as an update: a version that carries
     the file on disk is the same one. */
  g_assert_false (nuvashalauncher_modrinth_version_has_hash (
    update, "bdd6b8ac84b46ae56a3bbb761ce430c48e42e00d"));
  g_assert_true (nuvashalauncher_modrinth_version_has_hash (
    installed_version, "bdd6b8ac84b46ae56a3bbb761ce430c48e42e00d"));
  g_assert_true (nuvashalauncher_modrinth_version_has_hash (update, file->sha1));

  g_print ("ok - the hash update answer is another version of the same "
           "project\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/modrinth/facets_of_a_mod_search", test_facets_of_a_mod_search);
  g_test_add_func ("/modrinth/facets_of_a_quilt_search", test_facets_of_a_quilt_search);
  g_test_add_func ("/modrinth/facets_of_a_pack_search", test_facets_of_a_pack_search);
  g_test_add_func ("/modrinth/categories_filter_by_project_type", test_categories_filter_by_project_type);
  g_test_add_func ("/modrinth/category_labels", test_category_labels);
  g_test_add_func ("/modrinth/facets_with_categories", test_facets_with_categories);
  g_test_add_func ("/modrinth/search_url_paginates", test_search_url_paginates);
  g_test_add_func ("/modrinth/modpack_filters", test_modpack_filters_are_or_groups_and_sort);
  g_test_add_func ("/modrinth/search_results", test_search_results);
  g_test_add_func ("/modrinth/search_without_hits", test_search_without_hits);
  g_test_add_func ("/modrinth/search_rejects_malformed_hit", test_search_rejects_malformed_hit);
  g_test_add_func ("/modrinth/project_details", test_project_details);
  g_test_add_func ("/modrinth/versions_of_a_mod", test_versions_of_a_mod);
  g_test_add_func ("/modrinth/versions_of_a_pack", test_versions_of_a_pack);
  g_test_add_func ("/modrinth/versions_of_a_shader", test_versions_of_a_shader);
  g_test_add_func ("/modrinth/no_compatible_version", test_no_compatible_version);
  g_test_add_func ("/modrinth/version_by_hash", test_version_by_hash);
  g_test_add_func ("/modrinth/update_by_hash", test_update_by_hash);
  return g_test_run ();
}
