/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncher-minecraft-versions.
 *
 * Parsing and rule evaluation run against the JSON documents in
 * tests/fixtures. Fetching runs against a tiny libsoup server on the
 * loopback interface standing in for Mojang, so the tests need neither
 * the internet nor a display. The cache the module writes lives below
 * a temporary XDG_CACHE_HOME, set before anything can ask for the
 * real one. */

#include "nuvashalauncher-minecraft-versions.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#ifndef nuvashalauncher_TEST_FIXTURES
#define nuvashalauncher_TEST_FIXTURES "tests/fixtures"
#endif

/* The cache these tests write into. g_get_user_cache_dir() remembers
 * its first answer, so the environment is set before anything can ask
 * for the real one; cache_start() below does that. */
static char *cache_home;

/* ---------------------------------------------------------- *
 * Fixture documents
 * ---------------------------------------------------------- */

static inline inline char *
fixture_text (const char *name)
{
  g_autofree char *path = g_build_filename (nuvashalauncher_TEST_FIXTURES, name, NULL);
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("the fixture \"%s\" cannot be read: %s", name, error->message);

  return contents;
}

static inline nuvashalauncherVersionDescription *
fixture_version (const char *name)
{
  g_autofree char *text = fixture_text (name);
  g_autoptr (GError) error = NULL;
  nuvashalauncherVersionDescription *description;

  description = nuvashalauncher_version_description_new_from_json (text, -1, &error);

  if (description == NULL)
    g_error ("the fixture \"%s\" does not parse: %s", name, error->message);

  return description;
}

static inline inline const nuvashalauncherLibrary *
find_library (nuvashalauncherVersionDescription *description, const char *name)
{
  for (guint i = 0; i < description->libraries->len; i++)
    {
      nuvashalauncherLibrary *library = g_ptr_array_index (description->libraries, i);

      if (g_str_equal (library->name, name))
        return library;
    }

  g_error ("there is no library named \"%s\"", name);
  return NULL;
}

static inline inline char *
hash_of (const void *data, gsize length)
{
  return g_compute_checksum_for_data (G_CHECKSUM_SHA1, data, length);
}

/* ---------------------------------------------------------- *
 * Parsing the manifest
 * ---------------------------------------------------------- */





/* ---------------------------------------------------------- *
 * One version document, modern and legacy
 * ---------------------------------------------------------- */





/* ---------------------------------------------------------- *
 * Missing and malformed fields
 * ---------------------------------------------------------- */





/* ---------------------------------------------------------- *
 * Rules
 * ---------------------------------------------------------- */



/* ---------------------------------------------------------- *
 * Merging a profile with the version it inherits from
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

static inline inline char *
served_body (const ServedVersion *version)
{
  if (version->body != NULL)
    return g_strdup (version->body);

  return fixture_text (version->fixture);
}

static inline void
free_string (gpointer data)
{
  g_string_free (data, TRUE);
}

static inline void
serve (const char *path, const void *data, gsize length)
{
  g_hash_table_replace (fixture.files, g_strdup (path),
                        g_string_new_len (data, length));
}

static inline void
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
static inline inline char *
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

static inline inline char *
server_url (const char *path)
{
  return g_strconcat (fixture.base, path, NULL);
}

static inline void
fixture_start (void)
{
  g_autoptr (GError) error = NULL;
  GSList *uris = NULL;
  char *uri_string = NULL;

  fixture.files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                         free_string);
  fixture.server = soup_server_new ("server-header", "nuvashalauncher-test", NULL);

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

static inline void
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
  nuvashalauncherVersionManifest *manifest;
  nuvashalauncherVersionDescription *description;
  nuvashalauncherTaskScope *operation;
} AsyncCall;

static inline void
call_finished (AsyncCall *call)
{
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

/* A callback that never arrives fails the test instead of hanging. */
static inline gboolean
async_timeout (gpointer user_data)
{
  AsyncRun *run = user_data;

  run->timed_out = TRUE;
  g_main_loop_quit (run->loop);

  return G_SOURCE_REMOVE;
}

static inline void
async_run_init (AsyncRun *run)
{
  run->loop = g_main_loop_new (NULL, FALSE);
  run->cancellable = g_cancellable_new ();
}

static inline void
async_run_wait (AsyncRun *run)
{
  run->watchdog = g_timeout_add_seconds (30, async_timeout, run);

  if (run->pending > 0)
    g_main_loop_run (run->loop);

  if (!run->timed_out)
    g_source_remove (run->watchdog);

  g_assert_false (run->timed_out);
}

static inline void
async_run_clear (AsyncRun *run)
{
  if (run->loop != NULL)
    {
      g_main_loop_unref (run->loop);
      run->loop = NULL;
    }

  g_clear_object (&run->cancellable);
}

static inline void
async_call_clear (AsyncCall *call)
{
  g_clear_error (&call->error);
  g_clear_object (&call->manifest);
  g_clear_object (&call->description);
}

static inline void
manifest_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  AsyncCall *call = user_data;

  call->manifest = nuvashalauncher_version_manifest_fetch_finish (result, &call->error);
  call_finished (call);
}

static inline void
load_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  AsyncCall *call = user_data;

  call->description = nuvashalauncher_version_load_finish (result, &call->error);
  call_finished (call);
}

static inline void
call_manifest (AsyncRun *run, AsyncCall *call, const char *url)
{
  call->run = run;
  run->pending++;

  call->operation = nuvashalauncher_version_manifest_fetch_async (url, manifest_finished,
                                                      call);
  nuvashalauncher_task_scope_follow_cancellable (call->operation, run->cancellable);
}

static inline void
call_load (AsyncRun *run, AsyncCall *call, nuvashalauncherVersionManifest *manifest,
           const char *version_id)
{
  call->run = run;
  run->pending++;

  call->operation = nuvashalauncher_version_load_async (manifest, version_id,
                                             load_finished, call);
  nuvashalauncher_task_scope_follow_cancellable (call->operation, run->cancellable);
}

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

static inline inline char *
cache_file (const char *name)
{
  g_autofree char *directory =
    g_build_filename (g_get_user_cache_dir (), "nuvashalauncher", "versions", NULL);

  return g_build_filename (directory, name, NULL);
}



/* The manifest of the tests, parsed without going through the
 * network. */
static inline nuvashalauncherVersionManifest *
local_manifest (void)
{
  g_autofree char *json = manifest_json ();
  g_autoptr (GError) error = NULL;
  nuvashalauncherVersionManifest *manifest;

  manifest = nuvashalauncher_version_manifest_new_from_json (json, -1, &error);

  if (manifest == NULL)
    g_error ("the manifest of the test server does not parse: %s",
             error->message);

  return manifest;
}





/* ---------------------------------------------------------- *
 * The cache directory of these tests
 * ---------------------------------------------------------- */

/* g_get_user_cache_dir() remembers its first answer, so the
 * environment is set before anything can ask for it. */
static inline void
cache_start (void)
{
  g_autoptr (GError) error = NULL;

  cache_home = g_dir_make_tmp ("nuvashalauncher-versions-cache-XXXXXX", &error);

  if (cache_home == NULL)
    g_error ("the cache directory of the test cannot be made: %s",
             error->message);

  g_setenv ("XDG_CACHE_HOME", cache_home, TRUE);
}

static inline void
cache_stop (void)
{
  g_autofree char *directory =
     g_build_filename (cache_home, "nuvashalauncher", "versions", NULL);
  g_autofree char *nuvashalauncher_directory =
    g_build_filename (cache_home, "nuvashalauncher", NULL);
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
  g_rmdir (nuvashalauncher_directory);
  g_rmdir (cache_home);
}
