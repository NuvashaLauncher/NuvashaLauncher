#include "test-loaders-common.h"

static void
test_supported_loaders (void)
{
  const nuvashalauncherLoader *loaders = nuvashalauncher_loaders_supported ();
  guint count = nuvashalauncher_loaders_supported_count ();
  gboolean vanilla = FALSE;
  gboolean fabric = FALSE;

  /* Vanilla is what an instance is without a loader, so it is always in
     the list. */
  g_assert_cmpuint (count, >, 0);

  for (guint i = 0; i < count; i++)
    {
      g_assert_true (nuvashalauncher_loader_is_supported (loaders[i]));
      g_assert_nonnull (nuvashalauncher_loader_display_name (loaders[i]));

      if (loaders[i] == nuvashalauncher_LOADER_VANILLA)
        vanilla = TRUE;

      if (loaders[i] == nuvashalauncher_LOADER_FABRIC)
        fabric = TRUE;
    }

  g_assert_true (vanilla);
  g_assert_true (fabric);

  g_assert_cmpstr (nuvashalauncher_loader_display_name (nuvashalauncher_LOADER_VANILLA), ==, "Vanilla");
  g_assert_cmpstr (nuvashalauncher_loader_display_name (nuvashalauncher_LOADER_FABRIC), ==, "Fabric");
  g_assert_cmpstr (nuvashalauncher_loader_display_name (nuvashalauncher_LOADER_QUILT), ==, "Quilt");

  g_print ("ok - the loader lists hold the loaders nuvashalauncher installs\n");
}

static void
test_fabric_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/fabric-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = nuvashalauncher_loaders_versions_from_json (json, -1, &error);
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

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/quilt-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = nuvashalauncher_loaders_versions_from_json (json, -1, &error);
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
  versions = nuvashalauncher_loaders_versions_from_json ("[]", -1, &error);
  g_assert_null (versions);
  g_assert_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS);
  g_clear_error (&error);

  /* Something that is not a list at all is refused as well. */
  versions = nuvashalauncher_loaders_versions_from_json ("{\"error\": \"nope\"}", -1,
                                             &error);
  g_assert_null (versions);
  g_assert_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS);
  g_clear_error (&error);

  /* An entry that says nothing about a version is left out instead of
     becoming an empty row. */
  versions = nuvashalauncher_loaders_versions_from_json (
    "[{\"loader\": {}}, {\"loader\": {\"version\": \"0.19.5\"}}]", -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 1);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "0.19.5");

  g_print ("ok - an answer with nothing in it is refused, not shown\n");
}

static void
test_which_loaders_publish_an_installer (void)
{
  /* The two that serve a version document of their own and the two that
     publish an installer are installed in different ways, and the launcher
     asks which it is dealing with. */
  g_assert_false (nuvashalauncher_loader_publishes_an_installer (nuvashalauncher_LOADER_VANILLA));
  g_assert_false (nuvashalauncher_loader_publishes_an_installer (nuvashalauncher_LOADER_FABRIC));
  g_assert_false (nuvashalauncher_loader_publishes_an_installer (nuvashalauncher_LOADER_QUILT));
  g_assert_true (nuvashalauncher_loader_publishes_an_installer (nuvashalauncher_LOADER_NEOFORGE));
  g_assert_true (nuvashalauncher_loader_publishes_an_installer (nuvashalauncher_LOADER_FORGE));

  g_print ("ok - the loaders that publish an installer are told apart\n");
}

static void
test_compatibility_filtering (void)
{
  /* Forge: "1.20.1-47.2.0" runs 1.20.1 and nothing else, so a longer game
     version is not a prefix match. */
  g_assert_true (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_FORGE,
                                                "1.20.1-47.2.0", "1.20.1"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_FORGE,
                                                 "1.20.1-47.2.0", "1.20"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_FORGE,
                                                 "1.20.1-47.2.0", "1.20.2"));
  g_assert_true (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_FORGE,
                                                "1.21-51.0.33", "1.21"));

  /* NeoForge: "26.3.0.51-beta" runs 26.3, and a version of another game
     does not. */
  g_assert_true (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                "26.3.0.51-beta", "26.3"));
  g_assert_true (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                "21.1.256", "21.1"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                 "21.1.256", "21.11"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                 "20.2.3-beta", "26.3"));

  /* Modern NeoForge drops the leading "1." of a Minecraft version and
     writes a missing patch as 0, so the question is asked with the real
     Minecraft version the user picked. */
  g_assert_true (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                "21.1.256", "1.21.1"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                 "21.1.256", "1.21"));
  g_assert_true (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                "21.0.5", "1.21"));
  g_assert_true (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                "20.2.3-beta", "1.20.2"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                 "20.2.3-beta", "1.20.1"));

  /* A loader that publishes no installer is not asked the question: its
     versions are its own and name no game. */
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_FABRIC,
                                                 "0.19.5", "26.3"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE, NULL,
                                                 "26.3"));
  g_assert_false (nuvashalauncher_loaders_version_runs_game (nuvashalauncher_LOADER_NEOFORGE,
                                                 "26.3.0.51-beta", NULL));

  g_print ("ok - the versions of a loader are told apart by their names\n");
}

static void
test_neoforge_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/neoforge-versions.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  /* The maven holds every version NeoForge ever published, for every game;
     only the ones named after the game that was asked for are versions of
     it. */
  versions = nuvashalauncher_loaders_maven_versions_from_json (nuvashalauncher_LOADER_NEOFORGE, "26.3",
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
  versions = nuvashalauncher_loaders_maven_versions_from_json (nuvashalauncher_LOADER_NEOFORGE,
                                                    "21.1", json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 3);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "21.1.256");

  g_assert_true (version_at (versions, 0)->stable);
  g_assert_true (version_at (versions, 2)->stable);

  /* The same list asked for with the real Minecraft version the user
     picked, which NeoForge spells without the leading "1.". */
  g_clear_pointer (&versions, g_ptr_array_unref);
  versions = nuvashalauncher_loaders_maven_versions_from_json (nuvashalauncher_LOADER_NEOFORGE,
                                                   "1.21.1", json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 3);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "21.1.256");

  /* A game no version of NeoForge runs. */
  g_clear_pointer (&versions, g_ptr_array_unref);
  versions = nuvashalauncher_loaders_maven_versions_from_json (nuvashalauncher_LOADER_NEOFORGE,
                                                    "1.20.1", json, -1,
                                                    &error);
  g_assert_null (versions);
  g_assert_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS);

  g_print ("ok - a NeoForge maven is read and filtered by game version\n");
}

static void
test_forge_version_list (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/forge-versions.json", &json,
                            NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  versions = nuvashalauncher_loaders_maven_versions_from_json (nuvashalauncher_LOADER_FORGE, "1.20.1",
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
  versions = nuvashalauncher_loaders_maven_versions_from_json (nuvashalauncher_LOADER_FORGE, "1.16.5",
                                                    json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (versions->len, ==, 1);
  g_assert_cmpstr (version_at (versions, 0)->version, ==, "1.16.5-36.2.34");

  g_print ("ok - a Forge maven is read and filtered by game version\n");
}

static void
test_a_maven_that_answers_nothing (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  versions = nuvashalauncher_loaders_maven_versions_from_json (
    nuvashalauncher_LOADER_FORGE, "1.20.1", "{\"status\": 404}", -1, &error);
  g_assert_null (versions);
  g_assert_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS);

  g_print ("ok - a maven that answers nothing is refused\n");
}

static void
test_versions_for_display (void)
{
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) published = NULL;
  g_autoptr (GPtrArray) shown = NULL;
  guint selected = 0;

  if (!g_file_get_contents (nuvashalauncher_TEST_FIXTURES "/fabric-loader-list.json",
                            &json, NULL, &error))
    g_error ("the fixture cannot be read: %s", error->message);

  published = nuvashalauncher_loaders_versions_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (published);

  /* Stable only, and the newest stable version is picked. */
  shown = nuvashalauncher_loaders_versions_for_display (published, FALSE, NULL, &selected);
  g_assert_nonnull (shown);
  g_assert_cmpuint (shown->len, >, 0);
  g_assert_cmpuint (selected, ==, 0);
  g_assert_cmpstr (version_at (shown, selected)->version, ==, "0.19.5");

  for (guint i = 0; i < shown->len; i++)
    g_assert_true (version_at (shown, i)->stable);

  /* Asking for unstable versions shows the whole list. */
  g_clear_pointer (&shown, g_ptr_array_unref);
  shown = nuvashalauncher_loaders_versions_for_display (published, TRUE, NULL, &selected);
  g_assert_cmpuint (shown->len, ==, published->len);
  g_assert_cmpstr (version_at (shown, selected)->version, ==, "0.19.5");

  /* An instance that runs an unstable version keeps it even when the
     setting hides unstable versions, and that version is the pick. */
  g_clear_pointer (&shown, g_ptr_array_unref);
  shown = nuvashalauncher_loaders_versions_for_display (published, FALSE,
                                            "0.10.7+build.214", &selected);
  g_assert_cmpuint (selected, <, shown->len);
  g_assert_cmpstr (version_at (shown, selected)->version, ==,
                   "0.10.7+build.214");

  /* A list that holds only unstable versions, with unstable ones asked
     for, still picks its newest instead of nothing. */
  {
    static const char *const betas_ids[] = { "2.0.0-beta", "1.0.0-beta" };
    g_autoptr (GPtrArray) betas =
      g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_loader_version_free);

    for (guint i = 0; i < G_N_ELEMENTS (betas_ids); i++)
      {
        nuvashalauncherLoaderVersion *version = g_new0 (nuvashalauncherLoaderVersion, 1);

        version->version = g_strdup (betas_ids[i]);
        version->stable = FALSE;
        g_ptr_array_add (betas, version);
      }

    g_clear_pointer (&shown, g_ptr_array_unref);
    shown = nuvashalauncher_loaders_versions_for_display (betas, TRUE, NULL, &selected);
    g_assert_cmpuint (shown->len, ==, 2);
    g_assert_cmpuint (selected, ==, 0);
    g_assert_cmpstr (version_at (shown, 0)->version, ==, "2.0.0-beta");
  }

  {
    g_autoptr (GPtrArray) none = g_ptr_array_new ();

    g_clear_pointer (&shown, g_ptr_array_unref);
    shown = nuvashalauncher_loaders_versions_for_display (none, TRUE, NULL, &selected);
    g_assert_cmpuint (shown->len, ==, 0);
    g_assert_cmpuint (selected, ==, G_MAXUINT);
  }

  g_print ("ok - a list hides unstable versions but keeps a chosen one\n");
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
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
  return EXIT_SUCCESS;
}
