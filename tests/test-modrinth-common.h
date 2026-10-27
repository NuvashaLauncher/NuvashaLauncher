/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncher-modrinth.
 *
 * What nuvashalauncher has to get right when it reads Modrinth is not the network
 * but the two things around it: the facets that narrow a search to the
 * instance it is for, and the rule that keeps an incompatible version
 * out of every list. Both are decided from data here, so nothing in
 * this file needs the network.
 *
 * The fixtures are trimmed from what the API answered; the two entries
 * that only exist to exercise a filter (a version for another Minecraft
 * version and one for another loader) are shaped after real ones, and
 * the fixture that carries all four dependency kinds says so inside. */

#include "nuvashalauncher-content-page.h"
#include "nuvashalauncher-modrinth.h"

#include <glib.h>

#ifndef nuvashalauncher_TEST_FIXTURES
#define nuvashalauncher_TEST_FIXTURES "tests/fixtures"
#endif

static inline inline char *
read_fixture (const char *name)
{
  g_autofree char *path = g_build_filename (nuvashalauncher_TEST_FIXTURES, name, NULL);
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;

  if (!g_file_get_contents (path, &json, NULL, &error))
    g_error ("the fixture \"%s\" cannot be read: %s", name, error->message);

  return g_steal_pointer (&json);
}

/* A search for @version and @loader, with the strings it owns. */
static inline nuvashalauncherModrinthSearch *
search_new (nuvashalauncherContentType type, const char *query, const char *version,
            const char *loader)
{
  nuvashalauncherModrinthSearch *search = g_new0 (nuvashalauncherModrinthSearch, 1);

  search->type = type;
  search->query = g_strdup (query != NULL ? query : "");
  search->minecraft_version = g_strdup (version);
  search->loader = loader != NULL ? g_strdup (loader) : NULL;

  return search;
}

/* ---------------------------------------------------------- *
 * The facets of a search
 * ---------------------------------------------------------- */



/* Quilt runs Fabric mods, so a Quilt search asks Modrinth for both in one
 * facet: the arrays are an AND and the values of one are an OR. */






/* ---------------------------------------------------------- *
 * What a search answers
 * ---------------------------------------------------------- */







/* ---------------------------------------------------------- *
 * A project in full
 * ---------------------------------------------------------- */



/* ---------------------------------------------------------- *
 * Which versions fit
 * ---------------------------------------------------------- */

/* The tag list holds every project type; only the current page's type
 * and its readable grouping survive. */




/* Filtering by category puts the real slugs in the facets. */






/* The version of the fixture with this number, whatever its game
   versions: the one object of the answer nuvashalauncher would refuse is needed on
   its own to be asked about. (transfer full) */
static inline nuvashalauncherModrinthVersion *
version_from_fixture (const char *json, const char *number)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  JsonArray *array = NULL;
  guint length = 0;

  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);

  array = json_node_get_array (json_parser_get_root (parser));
  g_assert_nonnull (array);

  length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      JsonNode *item = json_array_get_element (array, i);
      JsonObject *object = json_node_get_object (item);
      const char *found = json_object_has_member (object, "version_number")
                              ? json_object_get_string_member (
                                  object, "version_number")
                              : NULL;

      if (g_strcmp0 (found, number) == 0)
        {
          g_autofree char *one = json_to_string (item, FALSE);

          return nuvashalauncher_modrinth_version_from_json (one, -1, &error);
        }
    }

  return NULL;
}

/* A shader is filtered by the Minecraft version and by nothing else.
 *
 * The fixture is what Modrinth answered for
 * complementary-reimagined: three versions whose loaders are "iris" and
 * "optifine", two of which list 1.20.1 and one of which does not. No
 * version of that project publishes "minecraft", which is why asking for
 * it by loader answered with nothing at all and a shader that plainly
 * supports the game version was reported as having none. */




/* ---------------------------------------------------------- *
 * A file on disk
 * ---------------------------------------------------------- */





/* ---------------------------------------------------------- *
 * What an install needs
 * ---------------------------------------------------------- */







/* ---------------------------------------------------------- *
 * The files of an instance
 * ---------------------------------------------------------- */



/* What the last scan of the test answered, and the loop that waits for
 * it. */
static GPtrArray *scanned = NULL;
static GMainLoop *scanning = NULL;

typedef struct
{
  nuvashalauncherTaskScope *operation;
} ScanWait;

static inline void
on_scan_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  ScanWait *wait = user_data;

  g_clear_pointer (&scanned, g_ptr_array_unref);
  scanned = nuvashalauncher_content_scan_finish (result, NULL);

  if (wait->operation != NULL)
    {
      nuvashalauncher_task_scope_unref (wait->operation);
      wait->operation = NULL;
    }
  g_free (wait);

  g_main_loop_quit (scanning);
}

/* Reads a content folder and waits for it, which is the asynchronous
 * way the page does the same. (transfer full) */
static inline GPtrArray *
scan (const char *instance, nuvashalauncherContentType type)
{
  GPtrArray *files = NULL;

  scanned = NULL;
  scanning = g_main_loop_new (NULL, FALSE);
  {
    /* On the heap: the answer arrives after this call returns, and the
       callback releases the handle it was given. */
    ScanWait *wait = g_new0 (ScanWait, 1);

    wait->operation =
      nuvashalauncher_content_scan_async (instance, type, on_scan_finished, wait);
  }
  g_main_loop_run (scanning);

  /* GMainLoop is refcounted but is not a GObject, so it cannot be given
   * to g_clear_object(). */
  g_main_loop_unref (scanning);
  scanning = NULL;

  files = scanned;
  scanned = NULL;

  return files;
}



/* ---------------------------------------------------------- *
 * The content kinds
 * ---------------------------------------------------------- */
