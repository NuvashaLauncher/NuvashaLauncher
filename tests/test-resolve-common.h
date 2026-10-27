/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* symlink(), like realpath(), only exists under glibc's defaults. */
#define _GNU_SOURCE

/* Tests for nuvashalauncher-minecraft-resolve.
 *
 * Maven paths, classifier choice, rule filtering, classpath order and
 * the arguments run against the documents in tests/fixtures. The
 * unpacking of the natives runs against jars written here with
 * libarchive. The whole resolve runs against a tiny libsoup server on
 * the loopback interface standing in for Mojang: it serves the manifest,
 * the version document, the jars, the asset index, the objects and the
 * logging file. Nothing needs the internet and nothing needs a display.
 * Every test writes below its own temporary directory. */

#include "nuvashalauncher-minecraft-resolve.h"

#include "nuvashalauncher-minecraft-versions.h"

#include <archive.h>
#include <archive_entry.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef nuvashalauncher_TEST_FIXTURES
#define nuvashalauncher_TEST_FIXTURES "tests/fixtures"
#endif

/* The version documents these tests resolve go to the cache directory of
   the versions module, so it is pointed at a temporary one before
   anything can ask for the real one. */
static char *cache_home;

/* ---------------------------------------------------------- *
 * The server standing in for Mojang
 * ---------------------------------------------------------- */

typedef struct
{
  SoupServer *server;
  char *base;        /* "http://127.0.0.1:port", no trailing slash */
  GHashTable *files; /* path -> GString * */
  guint hits;        /* requests seen, reset by every test */
} Fixture;

static Fixture fixture;

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
  GString *body;

  (void) server;
  (void) query;
  (void) user_data;

  fixture.hits++;

  body = g_hash_table_lookup (fixture.files, path);

  if (body == NULL)
    {
      soup_server_message_set_status (message, SOUP_STATUS_NOT_FOUND, NULL);
      return;
    }

  soup_server_message_set_status (message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (message, "application/octet-stream",
                                    SOUP_MEMORY_COPY, body->str, body->len);
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

  fixture.files =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, free_string);
  fixture.server = soup_server_new ("server-header", "nuvashalauncher-test", NULL);

  if (!soup_server_listen_local (fixture.server, 0, 0, &error))
    g_error ("the test server cannot listen: %s", error->message);

  soup_server_add_handler (fixture.server, "/", server_handler, NULL, NULL);

  uris = soup_server_get_uris (fixture.server);

  if (uris == NULL)
    g_error ("the test server has no address");

  uri_string = g_uri_to_string (uris->data);
  fixture.base = g_strdup (uri_string);

  if (fixture.base[0] != '\0')
    {
      gsize length = strlen (fixture.base);

      if (fixture.base[length - 1] == '/')
        fixture.base[length - 1] = '\0';
    }

  g_free (uri_string);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
}

static inline void
fixture_stop (void)
{
  g_clear_pointer (&fixture.files, g_hash_table_unref);
  g_clear_object (&fixture.server);
  g_clear_pointer (&fixture.base, g_free);
}

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
version_from_json (const char *json)
{
  g_autoptr (GError) error = NULL;
  nuvashalauncherVersionDescription *description =
    nuvashalauncher_version_description_new_from_json (json, -1, &error);

  g_assert_no_error (error);
  g_assert_nonnull (description);

  return description;
}

static inline nuvashalauncherVersionDescription *
fixture_version (const char *name)
{
  g_autofree char *text = fixture_text (name);

  return version_from_json (text);
}

static inline nuvashalauncherLibrary *
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
sha1_of (const void *data, gsize length)
{
  return g_compute_checksum_for_data (G_CHECKSUM_SHA1, data, length);
}

static inline inline char *
sha1_of_text (const char *text)
{
  return sha1_of (text, strlen (text));
}

static inline gboolean
holds (GPtrArray *arguments, const char *needle)
{
  for (guint i = 0; i < arguments->len; i++)
    {
      if (strstr (g_ptr_array_index (arguments, i), needle) != NULL)
        return TRUE;
    }

  return FALSE;
}

static inline inline char *
make_test_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("nuvashalauncher-resolve-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);

  return dir;
}

static inline void
cache_start (void)
{
  g_autoptr (GError) error = NULL;

  cache_home = g_dir_make_tmp ("nuvashalauncher-resolve-cache-XXXXXX", &error);

  if (cache_home == NULL)
    g_error ("the cache directory of the test cannot be made: %s",
             error->message);

  /* g_get_user_cache_dir() remembers its first answer, so this has to
     happen before anything asks for it. */
  g_setenv ("XDG_CACHE_HOME", cache_home, TRUE);
}

static inline void
cache_stop (void)
{
  g_autofree char *nuvashalauncher = g_build_filename (cache_home, "nuvashalauncher", NULL);
  g_autoptr (GDir) dir = g_dir_open (nuvashalauncher, 0, NULL);

  if (dir != NULL)
    {
      const char *name;

      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *path = g_build_filename (nuvashalauncher, name, NULL);

          g_remove (path);
        }
    }

  g_rmdir (nuvashalauncher);
  g_rmdir (cache_home);
}

static inline void
remove_test_dir (const char *path)
{
  g_autoptr (GPtrArray) children = g_ptr_array_new_with_free_func (g_free);
  GDir *dir = g_dir_open (path, 0, NULL);
  const char *name = NULL;

  if (dir == NULL)
    return;

  while ((name = g_dir_read_name (dir)) != NULL)
    g_ptr_array_add (children, g_build_filename (path, name, NULL));

  g_dir_close (dir);

  for (guint i = 0; i < children->len; i++)
    {
      const char *child = g_ptr_array_index (children, i);

      if (g_file_test (child, G_FILE_TEST_IS_DIR) &&
          !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        remove_test_dir (child);
      else
        g_remove (child);
    }

  g_rmdir (path);
}

/* ---------------------------------------------------------- *
 * One run of the main loop
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  guint pending;
  guint watchdog;
  gboolean timed_out;
} AsyncRun;

static inline void
async_run_init (AsyncRun *run)
{
  run->loop = g_main_loop_new (NULL, FALSE);
}

static inline gboolean
async_timeout (gpointer user_data)
{
  AsyncRun *run = user_data;

  run->timed_out = TRUE;
  g_main_loop_quit (run->loop);

  return G_SOURCE_REMOVE;
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
  g_main_loop_unref (run->loop);
  run->loop = NULL;
}

/* ---------------------------------------------------------- *
 * Maven paths
 * ---------------------------------------------------------- */



/* ---------------------------------------------------------- *
 * Entry names
 * ---------------------------------------------------------- */



/* ---------------------------------------------------------- *
 * Classifiers
 * ---------------------------------------------------------- */

/* A document whose only library publishes one natives jar per
   machine. */
static inline nuvashalauncherVersionDescription *
architecture_document (void)
{
  return version_from_json (
      "{\"id\": \"arch\", \"type\": \"release\", \"libraries\": ["
      " {\"name\": \"org.lwjgl:lwjgl:3.3.3\", \"downloads\": {\"classifiers\": {"
      "   \"natives-linux-x86_64\": {\"url\": \"http://x/x86_64.jar\","
      "                          \"sha1\": \"a\"},"
      "   \"natives-linux-arm64\": {\"url\": \"http://x/arm64.jar\","
      "                          \"sha1\": \"b\"},"
      "   \"natives-windows-x86\": {\"url\": \"http://x/win.jar\","
      "                          \"sha1\": \"c\"}"
      "  }}}]}");
}



/* ---------------------------------------------------------- *
 * Rules and classpath order
 * ---------------------------------------------------------- */

/* A library whose coordinate carries a classifier that is not a natives
   one is an ordinary jar, even when the rules name a single system. */
/* A classifier in the coordinate does not make a jar one to unpack:
   since 1.20.5 Mojang writes the natives as libraries of their own
   whose coordinate carries the classifier, and those jars belong on the
   classpath, because that is where LWJGL, JNA and netty find the
   natives they unpack themselves. Only the "natives" table of an older
   document says what is to be unpacked. */






/* ---------------------------------------------------------- *
 * Arguments
 * ---------------------------------------------------------- */



/* ---------------------------------------------------------- *
 * Jars to unpack
 * ---------------------------------------------------------- */

typedef struct
{
  const char *name;
  const char *body; /* NULL: a directory */
  gboolean link;
} JarEntry;

static inline void
write_jar (const char *path, const JarEntry *entries, guint count)
{
  struct archive *writer = archive_write_new ();

  g_assert_cmpint (archive_write_set_format_zip (writer), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);

  for (guint i = 0; i < count; i++)
    {
      struct archive_entry *entry = archive_entry_new ();
      gsize length = entries[i].body != NULL ? strlen (entries[i].body) : 0;
      la_ssize_t written = 0;

      archive_entry_set_pathname (entry, entries[i].name);

      if (entries[i].body == NULL)
        archive_entry_set_filetype (entry, AE_IFDIR);
      else if (entries[i].link)
        archive_entry_set_filetype (entry, AE_IFLNK);
      else
        archive_entry_set_filetype (entry, AE_IFREG);

      archive_entry_set_perm (entry, entries[i].body == NULL ? 0755 : 0644);
      archive_entry_set_size (entry, (la_int64_t) length);
      g_assert_cmpint (archive_write_header (writer, entry), ==, ARCHIVE_OK);

      if (entries[i].body != NULL && !entries[i].link)
        {
          written = archive_write_data (writer, entries[i].body, length);
          g_assert_cmpint (written, ==, (la_ssize_t) length);
        }

      archive_entry_free (entry);
    }

  g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_free (writer), ==, ARCHIVE_OK);
}

/* ---------------------------------------------------------- *
 * Unpacking the natives
 * ---------------------------------------------------------- */

typedef struct
{
  AsyncRun *run;
  gboolean success;
  GError *error;
  nuvashalauncherTaskScope *operation;
} ExtractCall;

static inline void
extract_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  ExtractCall *call = user_data;

  call->success = nuvashalauncher_resolve_extract_natives_finish (result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

/* Unpacks one jar into @natives_dir. */
static inline void
extract_one (const char *jar, const char *natives_dir, GPtrArray *exclude,
             ExtractCall *call)
{
  g_autoptr (GPtrArray) jars =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_resolve_file_free);
  nuvashalauncherResolveFile *file = g_new0 (nuvashalauncherResolveFile, 1);

  file->path = g_strdup (jar);
  file->natives = TRUE;
  file->exclude = exclude;

  g_ptr_array_add (jars, file);

  call->run->pending++;
  call->operation = nuvashalauncher_resolve_extract_natives_async (jars, natives_dir,
                                                       extract_finished, call);
}



/* A jar whose document lists no prefixes of its own still keeps the
   metadata out of the natives. */






/* ---------------------------------------------------------- *
 * The whole resolve, against the test server
 * ---------------------------------------------------------- */

typedef struct
{
  AsyncRun *run;
  nuvashalauncherVersionManifest *manifest;
  nuvashalauncherResolvedVersion *resolved;
  GError *error;
  guint progress_calls;
  guint last_done;
  guint last_total;
  char *last_stage;
  nuvashalauncherTaskScope *operation;
} ResolveCall;

static inline void
manifest_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  ResolveCall *call = user_data;

  call->manifest = nuvashalauncher_version_manifest_fetch_finish (result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

static inline void
resolve_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  ResolveCall *call = user_data;

  call->resolved = nuvashalauncher_resolve_version_finish (result, &call->error);
  nuvashalauncher_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

static inline void
resolve_progress (const nuvashalauncherResolveProgress *progress, gpointer user_data)
{
  ResolveCall *call = user_data;

  call->progress_calls++;
  call->last_done = progress->done;
  call->last_total = progress->total;
  g_free (call->last_stage);
  call->last_stage = g_strdup (progress->stage);
}

typedef struct
{
  const char *id;      /* the version id, also the name of its document */
  const char *assets;  /* what the document calls its asset index */
  GString *client;
  GString *library;
  GString *logging;
  GString *natives; /* the bytes of a real jar */
  GString *objects[2];
  char *hashes[2];
  char *paths[2]; /* the "path" of each object, NULL when it has none */
  gboolean virtual_index;
} Served;

/* The asset index the objects belong to. */
static inline inline char *
asset_index_json (const Served *served)
{
  GString *json = g_string_new ("{\"objects\": {");

  for (guint i = 0; i < G_N_ELEMENTS (served->objects); i++)
    {
      g_string_append_printf (
          json,
          "%s\"%s\": {\"hash\": \"%s\", \"size\": %u%s%s%s}",
          i > 0 ? "," : "", served->hashes[i], served->hashes[i],
          (guint) served->objects[i]->len,
          served->paths[i] != NULL ? ", \"path\": \"" : "",
          served->paths[i] != NULL ? served->paths[i] : "",
          served->paths[i] != NULL ? "\"" : "");
    }

  g_string_append (json, "}");

  if (served->virtual_index)
    g_string_append (json, ", \"virtual\": true, \"map_to_resources\": true");

  g_string_append (json, "}");

  return g_string_free (json, FALSE);
}

/* The version document, with the address of the test server in it. */
static inline inline char *
version_json (const Served *served)
{
  g_autofree char *index = asset_index_json (served);
  g_autofree char *index_sha1 = sha1_of_text (index);
  g_autofree char *client_sha1 =
    sha1_of (served->client->str, served->client->len);
  g_autofree char *library_sha1 =
    sha1_of (served->library->str, served->library->len);
  g_autofree char *logging_sha1 =
    sha1_of (served->logging->str, served->logging->len);
  g_autofree char *natives_sha1 =
    sha1_of (served->natives->str, served->natives->len);
  GString *json = g_string_new (NULL);

  g_string_append_printf (
      json,
      "{\"id\": \"%s\", \"type\": \"release\","
      " \"mainClass\": \"net.minecraft.client.main.Main\","
      " \"assets\": \"%s\","
      " \"assetIndex\": {\"id\": \"%s\", \"sha1\": \"%s\","
      "  \"url\": \"%s/index.json\"},"
      " \"downloads\": {\"client\": {\"sha1\": \"%s\","
      "  \"url\": \"%s/client.jar\"}},"
      " \"javaVersion\": {\"component\": \"java-runtime-delta\","
      "  \"majorVersion\": 21},"
      " \"logging\": {\"client\": {\"argument\":"
      "  \"-Dlog4j.configurationFile=${path}\", \"type\": \"log4j2-xml\","
      "  \"file\": {\"id\": \"client.xml\", \"sha1\": \"%s\","
      "            \"url\": \"%s/client.xml\"}}},"
      " \"arguments\": {\"jvm\": ["
      "   \"-Djava.library.path=${natives_directory}\", \"-cp\","
      "   \"${classpath}\","
      "   {\"rules\": [{\"action\": \"allow\", \"os\": {\"name\": \"osx\"}}],"
      "    \"value\": \"-XstartOnFirstThread\"}],"
      " \"game\": [\"--username\", \"${auth_player_name}\", \"--assetsDir\","
      " \"${assets_root}\", \"--assetIndex\", \"${assets_index_name}\"]},"
      " \"libraries\": ["
      " {\"name\": \"org.lwjgl:lwjgl:3.3.3\", \"downloads\": {\"artifact\":"
      "   {\"path\": \"org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar\","
      "    \"sha1\": \"%s\", \"url\": \"%s/lwjgl.jar\"}}},"
      " {\"name\": \"com.example:mac-only:1.0\", \"rules\":"
      "  [{\"action\": \"allow\", \"os\": {\"name\": \"osx\"}}],"
      "  \"downloads\": {\"artifact\": {\"path\":"
      "   \"com/example/mac-only/1.0/mac-only.jar\","
      "   \"sha1\": \"%s\", \"url\": \"%s/mac.jar\"}}},"
      " {\"name\": \"org.lwjgl:lwjgl:3.3.3:natives-linux\", \"downloads\":"
      "  {\"classifiers\": {\"natives-linux\": {\"path\":"
      "    \"org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3-natives-linux.jar\","
      "     \"sha1\": \"%s\", \"url\": \"%s/lwjgl-natives.jar\"}}},"
      "  \"natives\": {\"linux\": \"natives-linux\"},"
      "  \"extract\": {\"exclude\": [\"META-INF/\"]}}"
      "]}",
      served->id, served->assets, served->assets, index_sha1, fixture.base,
      client_sha1, fixture.base, logging_sha1, fixture.base, library_sha1,
      fixture.base, "0000000000000000000000000000000000000000", fixture.base,
      natives_sha1, fixture.base);

  return g_string_free (json, FALSE);
}

/* Publishes every file of @served on the test server. @jar_dir is where
   the real natives jar is written to be read back. */
static inline void
serve_version (Served *served, const char *jar_dir)
{
  static const JarEntry natives_entries[] = {
    { "META-INF/", NULL, FALSE },
    { "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n", FALSE },
    { "liblwjgl.so", "the lwjgl native\n", FALSE },
  };
  g_autofree char *jar_path = NULL;
  g_autofree char *index = NULL;
  g_autofree char *document = NULL;
  g_autofree char *document_path = NULL;
  g_autofree char *manifest = NULL;
  g_autofree char *document_sha1 = NULL;
  g_autofree char *manifest_path = NULL;

  /* The natives jar has to be a real one: it is unpacked. */
  jar_path = g_build_filename (jar_dir, "natives.jar", NULL);
  write_jar (jar_path, natives_entries, G_N_ELEMENTS (natives_entries));

  /* A jar is binary: its length is what the file says, not where the
     first zero byte is. */
  {
    g_autofree char *contents = NULL;
    gsize length = 0;

    g_assert_true (
        g_file_get_contents (jar_path, &contents, &length, NULL));
    g_string_append_len (served->natives, contents, (gssize) length);
  }

  serve ("/client.jar", served->client->str, served->client->len);
  serve ("/lwjgl.jar", served->library->str, served->library->len);
  serve ("/client.xml", served->logging->str, served->logging->len);
  serve ("/lwjgl-natives.jar", served->natives->str, served->natives->len);
  serve ("/mac.jar", "a jar of another system", 22);

  for (guint i = 0; i < G_N_ELEMENTS (served->objects); i++)
    {
      g_autofree char *bucket = g_strndup (served->hashes[i], 2);
      g_autofree char *path = NULL;

      path = g_strdup_printf ("/%s/%s", bucket, served->hashes[i]);
      serve (path, served->objects[i]->str, served->objects[i]->len);
    }

  index = asset_index_json (served);
  serve ("/index.json", index, strlen (index));

  document = version_json (served);
  document_path = g_strdup_printf ("/%s.json", served->id);
  serve (document_path, document, strlen (document));

  /* One manifest that knows this version and nothing else. */
  document_sha1 = sha1_of_text (document);
  manifest = g_strdup_printf (
      "{\"latest\": {\"release\": \"%s\", \"snapshot\": \"%s\"},"
      " \"versions\": [{\"id\": \"%s\", \"type\": \"release\","
      " \"url\": \"%s%s\", \"releaseTime\": \"2024-01-01T00:00:00+00:00\","
      " \"sha1\": \"%s\"}]}",
      served->id, served->id, served->id, fixture.base, document_path,
      document_sha1);
  manifest_path = g_build_filename (jar_dir, "manifest.json", NULL);
  g_file_set_contents (manifest_path, manifest, -1, NULL);
  serve ("/manifest.json", manifest, strlen (manifest));
}

static inline void
fetch_manifest (AsyncRun *run, ResolveCall *call, const char *url)
{
  call->run = run;
  run->pending++;

  call->operation = nuvashalauncher_version_manifest_fetch_async (url, manifest_finished,
                                                      call);
}

static inline void
start_resolve (AsyncRun *run, ResolveCall *call, nuvashalauncherVersionManifest *manifest,
               const char *version_id, const char *data_dir,
               const char *natives_dir, const char *asset_base_url)
{
  call->run = run;
  run->pending++;

  call->operation = nuvashalauncher_resolve_version_async (manifest, version_id, data_dir,
                                               natives_dir, asset_base_url,
                                               resolve_progress, call,
                                               resolve_finished, call);
}

static inline inline char *
read_text_file (const char *path)
{
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    g_error ("\"%s\" cannot be read", path);

  return contents;
}

static inline void
fill_served (Served *served, const char *id, const char *assets)
{
  memset (served, 0, sizeof (*served));
  served->id = id;
  served->assets = assets;
  served->client = g_string_new ("the client jar of the tests\n");
  served->library = g_string_new ("the lwjgl jar of the tests\n");
  served->logging = g_string_new ("<Configuration/>\n");
  served->natives = g_string_new (NULL);
  served->objects[0] = g_string_new ("the first asset\n");
  served->objects[1] = g_string_new ("the second asset\n");

  for (guint i = 0; i < G_N_ELEMENTS (served->objects); i++)
    served->hashes[i] = sha1_of (served->objects[i]->str,
                                 served->objects[i]->len);
}

static inline void
free_string_contents (gpointer data)
{
  if (data != NULL)
    g_string_free (data, TRUE);
}

static inline void
free_served (Served *served)
{
  g_clear_pointer (&served->client, free_string_contents);
  g_clear_pointer (&served->library, free_string_contents);
  g_clear_pointer (&served->logging, free_string_contents);
  g_clear_pointer (&served->natives, free_string_contents);

  for (guint i = 0; i < G_N_ELEMENTS (served->objects); i++)
    {
      g_clear_pointer (&served->objects[i], free_string_contents);
      g_clear_pointer (&served->hashes[i], g_free);
    }
}
