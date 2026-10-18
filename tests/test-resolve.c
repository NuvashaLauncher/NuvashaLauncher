/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* symlink(), like realpath(), only exists under glibc's defaults. */
#define _GNU_SOURCE

/* Tests for comfy-minecraft-resolve.
 *
 * Maven paths, classifier choice, rule filtering, classpath order and
 * the arguments run against the documents in tests/fixtures. The
 * unpacking of the natives runs against jars written here with
 * libarchive. The whole resolve runs against a tiny libsoup server on
 * the loopback interface standing in for Mojang: it serves the manifest,
 * the version document, the jars, the asset index, the objects and the
 * logging file. Nothing needs the internet and nothing needs a display.
 * Every test writes below its own temporary directory. */

#include "comfy-minecraft-resolve.h"

#include "comfy-minecraft-versions.h"

#include <archive.h>
#include <archive_entry.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef comfy_TEST_FIXTURES
#define comfy_TEST_FIXTURES "tests/fixtures"
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

  fixture.files =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, free_string);
  fixture.server = soup_server_new ("server-header", "comfy-test", NULL);

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

static void
fixture_stop (void)
{
  g_clear_pointer (&fixture.files, g_hash_table_unref);
  g_clear_object (&fixture.server);
  g_clear_pointer (&fixture.base, g_free);
}

/* ---------------------------------------------------------- *
 * Documents and small helpers
 * ---------------------------------------------------------- */

static char *
fixture_text (const char *name)
{
  g_autofree char *path = g_build_filename (comfy_TEST_FIXTURES, name, NULL);
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("the fixture \"%s\" cannot be read: %s", name, error->message);

  return contents;
}

static ComfyVersionDescription *
version_from_json (const char *json)
{
  g_autoptr (GError) error = NULL;
  ComfyVersionDescription *description =
    comfy_version_description_new_from_json (json, -1, &error);

  g_assert_no_error (error);
  g_assert_nonnull (description);

  return description;
}

static ComfyVersionDescription *
fixture_version (const char *name)
{
  g_autofree char *text = fixture_text (name);

  return version_from_json (text);
}

static ComfyLibrary *
find_library (ComfyVersionDescription *description, const char *name)
{
  for (guint i = 0; i < description->libraries->len; i++)
    {
      ComfyLibrary *library = g_ptr_array_index (description->libraries, i);

      if (g_str_equal (library->name, name))
        return library;
    }

  g_error ("there is no library named \"%s\"", name);
  return NULL;
}

static char *
sha1_of (const void *data, gsize length)
{
  return g_compute_checksum_for_data (G_CHECKSUM_SHA1, data, length);
}

static char *
sha1_of_text (const char *text)
{
  return sha1_of (text, strlen (text));
}

static gboolean
holds (GPtrArray *arguments, const char *needle)
{
  for (guint i = 0; i < arguments->len; i++)
    {
      if (strstr (g_ptr_array_index (arguments, i), needle) != NULL)
        return TRUE;
    }

  return FALSE;
}

static char *
make_test_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("comfy-resolve-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);

  return dir;
}

static void
cache_start (void)
{
  g_autoptr (GError) error = NULL;

  cache_home = g_dir_make_tmp ("comfy-resolve-cache-XXXXXX", &error);

  if (cache_home == NULL)
    g_error ("the cache directory of the test cannot be made: %s",
             error->message);

  /* g_get_user_cache_dir() remembers its first answer, so this has to
     happen before anything asks for it. */
  g_setenv ("XDG_CACHE_HOME", cache_home, TRUE);
}

static void
cache_stop (void)
{
  g_autofree char *comfy = g_build_filename (cache_home, "comfy", NULL);
  g_autoptr (GDir) dir = g_dir_open (comfy, 0, NULL);

  if (dir != NULL)
    {
      const char *name;

      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *path = g_build_filename (comfy, name, NULL);

          g_remove (path);
        }
    }

  g_rmdir (comfy);
  g_rmdir (cache_home);
}

static void
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

static void
async_run_init (AsyncRun *run)
{
  run->loop = g_main_loop_new (NULL, FALSE);
}

static gboolean
async_timeout (gpointer user_data)
{
  AsyncRun *run = user_data;

  run->timed_out = TRUE;
  g_main_loop_quit (run->loop);

  return G_SOURCE_REMOVE;
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
  g_main_loop_unref (run->loop);
  run->loop = NULL;
}

/* ---------------------------------------------------------- *
 * Maven paths
 * ---------------------------------------------------------- */

static void
test_maven_paths (void)
{
  g_autofree char *plain = comfy_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3");
  g_autofree char *classified =
    comfy_resolve_maven_path ("com.mojang:jtracy:1.14.38:natives-linux");
  g_autofree char *zip = comfy_resolve_maven_path (
      "de.oceanlabs.mcp:mcp_config:1.21-20240808.103037@zip");
  g_autofree char *added =
    comfy_resolve_maven_path_classified ("org.lwjgl.lwjgl:lwjgl-platform:2.9.0",
                                       "natives-linux");
  g_autofree char *with_extension = comfy_resolve_maven_path_classified (
      "org.lwjgl:lwjgl:3.3.3", "natives-linux@zip");
  g_autofree char *kept = comfy_resolve_maven_path_classified (
      "com.mojang:jtracy:1.14.38:natives-linux", "natives-windows");

  g_assert_cmpstr (plain, ==, "org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar");
  g_assert_cmpstr (classified, ==,
                   "com/mojang/jtracy/1.14.38/jtracy-1.14.38-natives-linux.jar");
  g_assert_cmpstr (zip, ==,
                   "de/oceanlabs/mcp/mcp_config/1.21-20240808.103037/"
                   "mcp_config-1.21-20240808.103037.zip");
  g_assert_cmpstr (added, ==,
                   "org/lwjgl/lwjgl/lwjgl-platform/2.9.0/"
                   "lwjgl-platform-2.9.0-natives-linux.jar");
  g_assert_cmpstr (with_extension, ==,
                   "org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3-natives-linux.zip");
  /* A coordinate that names a classifier keeps it. */
  g_assert_cmpstr (kept, ==,
                   "com/mojang/jtracy/1.14.38/jtracy-1.14.38-natives-linux.jar");

  /* Nothing that would step out of the libraries directory is a
     coordinate. */
  g_assert_null (comfy_resolve_maven_path ("org.lwjgl:lwjgl"));
  g_assert_null (comfy_resolve_maven_path ("org.lwjgl::3.3.3"));
  g_assert_null (comfy_resolve_maven_path ("org.lwjgl:lwjgl:"));
  g_assert_null (comfy_resolve_maven_path ("../evil:lwjgl:3.3.3"));
  g_assert_null (comfy_resolve_maven_path ("org.lwjgl:..:3.3.3"));
  g_assert_null (comfy_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3:../../x"));
  g_assert_null (comfy_resolve_maven_path ("org/lwjgl:lwjgl:3.3.3"));
  g_assert_null (comfy_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3@a/b"));
  g_assert_null (comfy_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3:natives/../x"));
  g_assert_null (comfy_resolve_maven_path (NULL));

  g_print ("ok - maven paths are built from coordinates\n");
}

/* ---------------------------------------------------------- *
 * Entry names
 * ---------------------------------------------------------- */

static void
test_entry_names (void)
{
  g_assert_true (comfy_resolve_entry_name_is_safe ("liblwjgl.so"));
  g_assert_true (comfy_resolve_entry_name_is_safe ("org/lwjgl/liblwjgl.so"));
  g_assert_true (comfy_resolve_entry_name_is_safe ("...weird name..."));

  g_assert_false (comfy_resolve_entry_name_is_safe (NULL));
  g_assert_false (comfy_resolve_entry_name_is_safe (""));
  g_assert_false (comfy_resolve_entry_name_is_safe ("/etc/passwd"));
  g_assert_false (comfy_resolve_entry_name_is_safe ("../escape.so"));
  g_assert_false (comfy_resolve_entry_name_is_safe ("a/../../escape.so"));
  g_assert_false (comfy_resolve_entry_name_is_safe ("a/.."));
  g_assert_false (comfy_resolve_entry_name_is_safe ("a//b.so"));
  g_assert_false (comfy_resolve_entry_name_is_safe ("a\\b.so"));
  g_assert_false (comfy_resolve_entry_name_is_safe ("a/./b.so"));

  g_print ("ok - only safe entry names are accepted\n");
}

/* ---------------------------------------------------------- *
 * Classifiers
 * ---------------------------------------------------------- */

/* A document whose only library publishes one natives jar per
   machine. */
static ComfyVersionDescription *
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

static void
test_native_classifiers (void)
{
  g_autoptr (ComfyVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (ComfyVersionDescription) arch = architecture_document ();
  g_autoptr (ComfyRuleContext) x86 =
    comfy_rule_context_new_for ("linux", "x86_64", NULL);
  g_autoptr (ComfyRuleContext) arm =
    comfy_rule_context_new_for ("linux", "aarch64", NULL);
  g_autoptr (ComfyRuleContext) other =
    comfy_rule_context_new_for ("linux", "riscv64", NULL);
  g_autoptr (ComfyRuleContext) windows =
    comfy_rule_context_new_for ("windows", "x86_64", NULL);
  ComfyLibrary *platform =
    find_library (legacy, "org.lwjgl.lwjgl:lwjgl-platform:2.9.0");
  ComfyLibrary *scala =
    find_library (legacy, "org.scala-lang:scala-library:2.10.2");
  ComfyLibrary *per_machine = find_library (arch, "org.lwjgl:lwjgl:3.3.3");

  /* The natives table names the classifier directly. */
  g_assert_cmpstr (comfy_resolve_native_classifier (platform, x86), ==,
                   "natives-linux");
  /* Another system has its own classifier in the same table. */
  g_assert_cmpstr (comfy_resolve_native_classifier (platform, windows), ==,
                   "natives-windows");
  /* A library that publishes no classifier has no natives to pick. */
  g_assert_null (comfy_resolve_native_classifier (scala, x86));

  /* One jar per machine: the architecture decides the name, with every
     spelling that is published for it tried. */
  g_assert_cmpstr (comfy_resolve_native_classifier (per_machine, x86), ==,
                   "natives-linux-x86_64");
  g_assert_cmpstr (comfy_resolve_native_classifier (per_machine, arm), ==,
                   "natives-linux-arm64");
  /* Nothing was published for this machine, so nothing is invented. */
  g_assert_null (comfy_resolve_native_classifier (per_machine, other));
  /* The names of another system are not picked up. */
  g_assert_null (comfy_resolve_native_classifier (per_machine, windows));

  g_print ("ok - the natives classifier of this machine is chosen\n");
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
static void
test_classifier_is_not_natives (void)
{
  g_autoptr (ComfyVersionDescription) description = version_from_json (
      "{\"id\": \"x\", \"type\": \"release\", \"libraries\": ["
      " {\"name\": \"io.netty:netty-transport-native-epoll:4.2.16:linux-x86_64\","
      "  \"downloads\": {\"artifact\": {\"path\":"
      "   \"io/netty/epoll/4.2.16/epoll-linux-x86_64.jar\","
      "   \"sha1\": \"aa\", \"url\": \"http://x/epoll.jar\"}}},"
      " {\"name\": \"org.lwjgl:lwjgl:3.4.3:natives-linux\","
      "  \"downloads\": {\"artifact\": {\"path\":"
      "   \"org/lwjgl/lwjgl/3.4.3/lwjgl-3.4.3-natives-linux.jar\","
      "   \"sha1\": \"bb\", \"url\": \"http://x/lwjgl-natives.jar\"}}}]}");
  g_autoptr (ComfyRuleContext) context =
    comfy_rule_context_new_for ("linux", "x86_64", NULL);
  g_autofree char *libraries =
    g_build_filename ("/data", "libraries", NULL);
  g_autofree char *classpath =
    comfy_resolve_classpath (description, context, libraries, NULL);
  g_autoptr (GPtrArray) files =
    comfy_resolve_library_files (description, context, libraries);
  g_auto (GStrv) entries = NULL;

  g_assert_cmpuint (description->libraries->len, ==, 2);
  g_assert_cmpuint (files->len, ==, 2);

  /* Neither of them is a jar to unpack. */
  g_assert_false (((ComfyResolveFile *) g_ptr_array_index (files, 0))->natives);
  g_assert_false (((ComfyResolveFile *) g_ptr_array_index (files, 1))->natives);

  entries = g_strsplit (classpath, ":", -1);
  g_assert_cmpuint (g_strv_length (entries), ==, 2);
  g_assert_cmpstr (entries[0], ==,
                   "/data/libraries/io/netty/epoll/4.2.16/"
                   "epoll-linux-x86_64.jar");
  g_assert_cmpstr (entries[1], ==,
                   "/data/libraries/org/lwjgl/lwjgl/3.4.3/"
                   "lwjgl-3.4.3-natives-linux.jar");

  g_print ("ok - a coordinate classifier does not make a jar one to unpack\n");
}

static void
test_rule_filtering (void)
{
  g_autoptr (ComfyVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (ComfyRuleContext) linux =
    comfy_rule_context_new_for ("linux", "x86_64", NULL);
  g_autoptr (ComfyRuleContext) mac =
    comfy_rule_context_new_for ("osx", "x86_64", NULL);
  ComfyLibrary *windows_only =
    find_library (legacy, "com.example:windows-only:1.0");
  ComfyLibrary *linux_only = find_library (legacy, "com.example:linux-only:1.0");
  ComfyLibrary *scala =
    find_library (legacy, "org.scala-lang:scala-library:2.10.2");
  g_autofree char *libraries =
    g_build_filename ("/data", "libraries", NULL);
  g_autoptr (GPtrArray) files = NULL;

  /* A rule that names another system does not match, and when no rule
     matches the library is out. */
  g_assert_false (comfy_library_allowed (windows_only, linux));
  g_assert_false (comfy_library_allowed (windows_only, mac));
  g_assert_true (comfy_library_allowed (linux_only, linux));
  g_assert_false (comfy_library_allowed (linux_only, mac));
  /* No rules at all means allowed everywhere. */
  g_assert_true (comfy_library_allowed (scala, mac));

  /* Only the libraries that apply here are planned, and a library that
     names no jar of its own contributes nothing. */
  files = comfy_resolve_library_files (legacy, linux, libraries);
  g_assert_nonnull (files);
  g_assert_cmpuint (files->len, ==, 2); /* the scala jar, the natives jar */

  for (guint i = 0; i < files->len; i++)
    {
      const ComfyResolveFile *file = g_ptr_array_index (files, i);

      g_assert_nonnull (file->url);
      g_assert_nonnull (file->path);
      g_assert_true (g_str_has_prefix (file->path, "/data/libraries/"));
      g_assert_null (strstr (file->path, "windows-only"));
      g_assert_null (strstr (file->path, "linux-only"));
    }

  /* The one natives jar of the document is marked as one, and it is the
     first library of the document that has a jar at all. */
  {
    const ComfyResolveFile *file = g_ptr_array_index (files, 0);

    g_assert_true (file->natives);
    g_assert_cmpstr (g_ptr_array_index (file->exclude, 0), ==, "META-INF/");
    g_assert_false (((ComfyResolveFile *) g_ptr_array_index (files, 1))->natives);
  }

  g_print ("ok - rules decide which libraries this machine uses\n");
}

static void
test_classpath_order (void)
{
  g_autoptr (ComfyVersionDescription) modern =
    fixture_version ("version-modern.json");
  g_autoptr (ComfyVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (ComfyRuleContext) context =
    comfy_rule_context_new_for ("linux", "x86_64", NULL);
  g_autofree char *libraries =
    g_build_filename ("/data", "libraries", NULL);
  g_autofree char *client =
    g_build_filename ("/data", "versions", "1.21.1", "1.21.1.jar", NULL);
  g_autofree char *classpath = NULL;
  g_autofree char *legacy_classpath = NULL;
  g_auto (GStrv) entries = NULL;

  classpath = comfy_resolve_classpath (modern, context, libraries, client);
  g_assert_nonnull (classpath);

  entries = g_strsplit (classpath, ":", -1);
  g_assert_cmpuint (g_strv_length (entries), ==, 4);

  /* The libraries of the document, in the order it lists them. The
     library of another system is left out. jtracy is written as a
     library whose coordinate carries the classifier, which since 1.20.5
     is a jar for the classpath: the document tells LWJGL and JNA where
     to unpack the natives they find in it. */
  g_assert_cmpstr (entries[0], ==,
                   "/data/libraries/org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar");
  g_assert_cmpstr (entries[1], ==,
                   "/data/libraries/com/mojang/jtracy/1.14.38/"
                   "jtracy-1.14.38-natives-linux.jar");
  g_assert_cmpstr (entries[2], ==,
                   "/data/libraries/org/lwjgl/lwjgl-glfw/3.3.3/"
                   "lwjgl-glfw-3.3.3.jar");
  g_assert_null (strstr (classpath, "java-objc-bridge"));
  /* The client jar goes last. */
  g_assert_cmpstr (entries[3], ==, client);

  legacy_classpath = comfy_resolve_classpath (legacy, context, libraries, client);

  g_strfreev (entries);
  entries = g_strsplit (legacy_classpath, ":", -1);
  g_assert_cmpuint (g_strv_length (entries), ==, 2);
  g_assert_cmpstr (entries[0], ==,
                   "/data/libraries/org/scala-lang/scala-library/2.10.2/"
                   "scala-library-2.10.2.jar");
  g_assert_cmpstr (entries[1], ==, client);

  /* A natives jar is unpacked, not put on the classpath. */
  g_assert_null (strstr (legacy_classpath, "natives-linux"));
  g_assert_null (strstr (legacy_classpath, "windows-only"));
  g_assert_null (strstr (legacy_classpath, "linux-only"));

  g_print ("ok - the classpath holds the libraries in order, client last\n");
}

/* ---------------------------------------------------------- *
 * Arguments
 * ---------------------------------------------------------- */

static void
test_arguments (void)
{
  g_autoptr (ComfyVersionDescription) modern =
    fixture_version ("version-modern.json");
  g_autoptr (ComfyVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (ComfyRuleContext) context =
    comfy_rule_context_new_for ("linux", "x86_64", NULL);
  g_autoptr (GPtrArray) jvm = comfy_resolve_jvm_arguments (modern, context);
  g_autoptr (GPtrArray) game = comfy_resolve_game_arguments (modern, context);
  g_autoptr (GPtrArray) old_game =
    comfy_resolve_game_arguments (legacy, context);

  /* The jvm arguments of this machine: the plain ones plus the one that
     asks for a bigger stack. The ones for macOS, for Windows and for a
     demo account are left out. */
  g_assert_cmpuint (jvm->len, ==, 4);
  g_assert_cmpstr (g_ptr_array_index (jvm, 0), ==,
                   "-Djava.library.path=${natives_directory}");
  g_assert_cmpstr (g_ptr_array_index (jvm, 2), ==, "${classpath}");
  g_assert_cmpstr (g_ptr_array_index (jvm, 3), ==, "-Xss1M");

  g_assert_cmpuint (game->len, ==, 8);
  g_assert_cmpstr (g_ptr_array_index (game, 0), ==, "--username");
  g_assert_cmpstr (g_ptr_array_index (game, 1), ==, "${auth_player_name}");
  g_assert_false (holds (game, "--demo"));
  g_assert_true (holds (game, "--assetsDir"));

  /* A version old enough to have one line of arguments instead of an
     array keeps it, placeholders and all. */
  g_assert_cmpuint (old_game->len, ==, 12);
  g_assert_cmpstr (g_ptr_array_index (old_game, 0), ==, "--username");
  g_assert_cmpstr (g_ptr_array_index (old_game, 10), ==, "--tweakClass");

  g_print ("ok - the arguments of this machine are kept in order\n");
}

/* ---------------------------------------------------------- *
 * Jars to unpack
 * ---------------------------------------------------------- */

typedef struct
{
  const char *name;
  const char *body; /* NULL: a directory */
  gboolean link;
} JarEntry;

static void
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
  ComfyTaskScope *operation;
} ExtractCall;

static void
extract_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  ExtractCall *call = user_data;

  call->success = comfy_resolve_extract_natives_finish (result, &call->error);
  comfy_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

/* Unpacks one jar into @natives_dir. */
static void
extract_one (const char *jar, const char *natives_dir, GPtrArray *exclude,
             ExtractCall *call)
{
  g_autoptr (GPtrArray) jars =
    g_ptr_array_new_with_free_func ((GDestroyNotify) comfy_resolve_file_free);
  ComfyResolveFile *file = g_new0 (ComfyResolveFile, 1);

  file->path = g_strdup (jar);
  file->natives = TRUE;
  file->exclude = exclude;

  g_ptr_array_add (jars, file);

  call->run->pending++;
  call->operation = comfy_resolve_extract_natives_async (jars, natives_dir,
                                                       extract_finished, call);
}

static void
test_natives_extraction (void)
{
  static const JarEntry entries[] = {
    { "META-INF/", NULL, FALSE },
    { "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n", FALSE },
    { "META-INF/nested/thing.txt", "excluded too\n", FALSE },
    { "liblwjgl.so", "the lwjgl native", FALSE },
    { "org/lwjgl/libopenal.so", "the openal native", FALSE },
    { "empty.txt", "", FALSE },
  };
  g_autofree char *dir = make_test_dir ();
  g_autofree char *jar = g_build_filename (dir, "natives.jar", NULL);
  g_autofree char *natives = g_build_filename (dir, "natives", NULL);
  g_autofree char *path = NULL;
  g_autofree char *text = NULL;
  g_autoptr (GPtrArray) exclude = g_ptr_array_new_with_free_func (g_free);
  AsyncRun run = { 0 };
  ExtractCall call = { 0 };

  g_ptr_array_add (exclude, g_strdup ("META-INF/"));

  g_assert_cmpint (g_mkdir_with_parents (natives, 0755), ==, 0);
  write_jar (jar, entries, G_N_ELEMENTS (entries));

  call.run = &run;
  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_true (call.success);

  /* Nothing of the excluded directory was written. */
  g_free (path);
  path = g_build_filename (natives, "META-INF", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  g_free (path);
  path = g_build_filename (natives, "liblwjgl.so", NULL);
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "the lwjgl native");

  /* A nested entry makes its directory. */
  g_clear_pointer (&text, g_free);
  g_free (path);
  path = g_build_filename (natives, "org", "lwjgl", "libopenal.so", NULL);
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "the openal native");

  /* An empty entry is an empty file. */
  g_clear_pointer (&text, g_free);
  g_free (path);
  path = g_build_filename (natives, "empty.txt", NULL);
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "");

  g_clear_error (&call.error);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - natives are unpacked, excluded entries are left out\n");
}

/* A jar whose document lists no prefixes of its own still keeps the
   metadata out of the natives. */
static void
test_default_exclude (void)
{
  static const JarEntry entries[] = {
    { "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n", FALSE },
    { "META-INF/versions/9/module-info.class", "not code\n", FALSE },
    { "libexample.so", "the example native\n", FALSE },
  };
  g_autofree char *dir = make_test_dir ();
  g_autofree char *jar = g_build_filename (dir, "natives.jar", NULL);
  g_autofree char *natives = g_build_filename (dir, "natives", NULL);
  g_autofree char *path = NULL;
  g_autoptr (GPtrArray) exclude = NULL;
  AsyncRun run = { 0 };
  ExtractCall call = { 0 };

  g_assert_cmpint (g_mkdir_with_parents (natives, 0755), ==, 0);
  write_jar (jar, entries, G_N_ELEMENTS (entries));

  /* No exclude list at all, which is what a library written since 1.20.5
     comes with. */
  exclude = NULL;
  call.run = &run;

  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_true (call.success);

  path = g_build_filename (natives, "META-INF", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  g_free (path);
  path = g_build_filename (natives, "libexample.so", NULL);
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a jar with no exclude list still keeps META-INF out\n");
}

static void
test_malicious_jar (void)
{
  static const JarEntry walking[] = {
    { "lib.so", "a real entry", FALSE },
    { "../escaped.so", "outside", FALSE },
  };
  static const JarEntry absolute[] = {
    { "/tmp/comfy-resolve-escaped.so", "outside", FALSE },
  };
  static const JarEntry nested[] = {
    { "sub/../../escaped.so", "outside", FALSE },
  };
  static const JarEntry links[] = {
    { "linked.so", "somewhere else", TRUE },
  };
  g_autofree char *dir = make_test_dir ();
  g_autofree char *jar = g_build_filename (dir, "evil.jar", NULL);
  g_autofree char *natives = g_build_filename (dir, "natives", NULL);
  g_autofree char *path = NULL;
  g_autoptr (GPtrArray) exclude = NULL;
  AsyncRun run = { 0 };
  ExtractCall call = { 0 };

  g_assert_cmpint (g_mkdir_with_parents (natives, 0755), ==, 0);
  exclude = g_ptr_array_new_with_free_func (g_free);
  call.run = &run;

  /* An entry that walks out of the directory fails the whole jar, and
     what came before it is left alone. */
  write_jar (jar, walking, G_N_ELEMENTS (walking));
  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);
  async_run_clear (&run);

  g_assert_null (call.success);
  g_assert_error (call.error, comfy_RESOLVE_ERROR,
                  comfy_RESOLVE_ERROR_UNSAFE_PATH);
  {
    g_autofree char *escaped = g_build_filename (dir, "escaped.so", NULL);

    g_assert_false (g_file_test (escaped, G_FILE_TEST_EXISTS));
  }
  g_clear_error (&call.error);

  /* So does an absolute name. */
  write_jar (jar, absolute, G_N_ELEMENTS (absolute));
  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);
  async_run_clear (&run);

  g_assert_null (call.success);
  g_assert_error (call.error, comfy_RESOLVE_ERROR,
                  comfy_RESOLVE_ERROR_UNSAFE_PATH);
  g_assert_false (g_file_test ("/tmp/comfy-resolve-escaped.so",
                               G_FILE_TEST_EXISTS));
  g_clear_error (&call.error);

  write_jar (jar, nested, G_N_ELEMENTS (nested));
  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);
  async_run_clear (&run);

  g_assert_null (call.success);
  g_assert_error (call.error, comfy_RESOLVE_ERROR,
                  comfy_RESOLVE_ERROR_UNSAFE_PATH);
  g_clear_error (&call.error);

  /* A link inside a jar is skipped: honouring one would put a name of
     the natives directory somewhere else on the disk. */
  write_jar (jar, links, G_N_ELEMENTS (links));
  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);
  async_run_clear (&run);

  g_assert_no_error (call.error);
  g_assert_true (call.success);
  g_free (path);
  path = g_build_filename (natives, "linked.so", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* What the earlier jar left behind is untouched. */
  g_free (path);
  path = g_build_filename (natives, "lib.so", NULL);
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  remove_test_dir (dir);

  g_print ("ok - a jar cannot write outside the natives directory\n");
}

static void
test_symlinked_directory (void)
{
  static const JarEntry entries[] = {
    { "escape/planted.so", "planted", FALSE },
  };
  g_autofree char *dir = make_test_dir ();
  g_autofree char *jar = g_build_filename (dir, "natives.jar", NULL);
  g_autofree char *natives = g_build_filename (dir, "natives", NULL);
  g_autofree char *elsewhere = g_build_filename (dir, "elsewhere", NULL);
  g_autofree char *link = NULL;
  g_autoptr (GPtrArray) exclude = NULL;
  AsyncRun run = { 0 };
  ExtractCall call = { 0 };

  g_assert_cmpint (g_mkdir_with_parents (natives, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (elsewhere, 0755), ==, 0);

  link = g_build_filename (natives, "escape", NULL);
  g_assert_cmpint (symlink (elsewhere, link), ==, 0);

  write_jar (jar, entries, G_N_ELEMENTS (entries));
  exclude = g_ptr_array_new_with_free_func (g_free);
  call.run = &run;

  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);
  async_run_clear (&run);

  /* A directory of the natives that is a link out of it: nothing was
     written through it. */
  g_assert_null (call.success);
  g_assert_nonnull (call.error);
  {
    g_autofree char *planted = g_build_filename (elsewhere, "planted.so", NULL);

    g_assert_false (g_file_test (planted, G_FILE_TEST_EXISTS));
  }

  g_clear_error (&call.error);
  remove_test_dir (dir);

  g_print ("ok - a symlinked directory of the natives is refused\n");
}

/* ---------------------------------------------------------- *
 * The whole resolve, against the test server
 * ---------------------------------------------------------- */

typedef struct
{
  AsyncRun *run;
  ComfyVersionManifest *manifest;
  ComfyResolvedVersion *resolved;
  GError *error;
  guint progress_calls;
  guint last_done;
  guint last_total;
  char *last_stage;
  ComfyTaskScope *operation;
} ResolveCall;

static void
manifest_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  ResolveCall *call = user_data;

  call->manifest = comfy_version_manifest_fetch_finish (result, &call->error);
  comfy_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

static void
resolve_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  ResolveCall *call = user_data;

  call->resolved = comfy_resolve_version_finish (result, &call->error);
  comfy_task_scope_unref (call->operation);
  call->operation = NULL;

  if (--call->run->pending == 0)
    g_main_loop_quit (call->run->loop);
}

static void
resolve_progress (const ComfyResolveProgress *progress, gpointer user_data)
{
  ResolveCall *call = user_data;

  call->progress_calls++;
  call->last_done = progress->done;
  call->last_total = progress->total;
  g_free (call->last_stage);
  call->last_stage = g_strdup (progress->stage);
}

/* Everything the server has to answer with for one resolve. */
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
static char *
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
static char *
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
static void
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

static void
fetch_manifest (AsyncRun *run, ResolveCall *call, const char *url)
{
  call->run = run;
  run->pending++;

  call->operation = comfy_version_manifest_fetch_async (url, manifest_finished,
                                                      call);
}

static void
start_resolve (AsyncRun *run, ResolveCall *call, ComfyVersionManifest *manifest,
               const char *version_id, const char *data_dir,
               const char *natives_dir, const char *asset_base_url)
{
  call->run = run;
  run->pending++;

  call->operation = comfy_resolve_version_async (manifest, version_id, data_dir,
                                               natives_dir, asset_base_url,
                                               resolve_progress, call,
                                               resolve_finished, call);
}

static char *
read_text_file (const char *path)
{
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    g_error ("\"%s\" cannot be read", path);

  return contents;
}

static void
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

static void
free_string_contents (gpointer data)
{
  if (data != NULL)
    g_string_free (data, TRUE);
}

static void
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

static void
test_resolve_modern (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *natives_dir = g_build_filename (dir, "instance", NULL);
  g_autofree char *assets_url = g_strdup (fixture.base);
  g_autofree char *client_jar = NULL;
  g_autofree char *library_jar = NULL;
  g_autofree char *natives_jar = NULL;
  g_autofree char *native_file = NULL;
  g_autofree char *logging_file = NULL;
  g_autofree char *object = NULL;
  g_autofree char *path = NULL;
  g_autofree char *text = NULL;
  g_autofree char *classpath = NULL;
  AsyncRun run = { 0 };
  ResolveCall call = { 0 };
  Served served;

  g_autofree char *manifest_url = server_url ("/manifest.json");
  fill_served (&served, "test-version", "test");
  serve_version (&served, dir);

  async_run_init (&run);
  fetch_manifest (&run, &call, manifest_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);
  g_assert_cmpuint (fixture.hits, ==, 1);

  start_resolve (&run, &call, call.manifest, "test-version", data_dir,
                 natives_dir, assets_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.resolved);

  /* Everything the document named is on the disk where it belongs. */
  client_jar = g_build_filename (data_dir, "versions", "test-version",
                                 "test-version.jar", NULL);
  library_jar = g_build_filename (data_dir, "libraries", "org", "lwjgl",
                                  "lwjgl", "3.3.3", "lwjgl-3.3.3.jar", NULL);
  natives_jar = g_build_filename (data_dir, "libraries", "org", "lwjgl",
                                  "lwjgl", "3.3.3",
                                  "lwjgl-3.3.3-natives-linux.jar", NULL);
  /* The logging file keeps the name the document gave it. */
  logging_file = g_build_filename (data_dir, "logging", "client.xml", NULL);

  g_free (text);
  text = read_text_file (client_jar);
  g_assert_cmpstr (text, ==, served.client->str);
  g_clear_pointer (&text, g_free);

  g_free (text);
  text = read_text_file (library_jar);
  g_assert_cmpstr (text, ==, served.library->str);
  g_clear_pointer (&text, g_free);

  g_assert_true (g_file_test (natives_jar, G_FILE_TEST_EXISTS));

  /* The library of another system was never asked for. */
  g_free (path);
  path = g_build_filename (data_dir, "libraries", "com", "example", "mac-only",
                           "1.0", "mac-only.jar", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* The natives jar was unpacked, without what it excludes. */
  native_file = g_build_filename (natives_dir, "liblwjgl.so", NULL);
  g_free (text);
  text = read_text_file (native_file);
  g_assert_cmpstr (text, ==, "the lwjgl native\n");
  g_clear_pointer (&text, g_free);

  g_free (path);
  path = g_build_filename (natives_dir, "META-INF", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* The asset objects are named after their own hash. */
  for (guint i = 0; i < G_N_ELEMENTS (served.objects); i++)
    {
      g_autofree char *bucket = g_strndup (served.hashes[i], 2);

      g_free (object);
      object = g_build_filename (data_dir, "assets", "objects", bucket,
                                 served.hashes[i], NULL);
      text = read_text_file (object);
      g_assert_cmpstr (text, ==, served.objects[i]->str);
      g_clear_pointer (&text, g_free);
    }

  /* And the answer says so. */
  g_assert_cmpstr (call.resolved->version_id, ==, "test-version");
  g_assert_cmpstr (call.resolved->version_type, ==, "release");
  g_assert_cmpstr (call.resolved->main_class, ==,
                   "net.minecraft.client.main.Main");
  g_assert_cmpstr (call.resolved->client_jar, ==, client_jar);
  g_assert_cmpstr (call.resolved->natives_directory, ==, natives_dir);
  g_free (path);
  path = g_build_filename (data_dir, "assets", NULL);
  g_assert_cmpstr (call.resolved->assets_directory, ==, path);
  g_assert_cmpstr (call.resolved->assets_index_name, ==, "test");
  g_assert_cmpint (call.resolved->java_major_version, ==, 21);

  classpath = g_strdup_printf ("%s:%s", library_jar, client_jar);
  g_assert_cmpstr (call.resolved->classpath, ==, classpath);

  g_free (path);
  path = g_strdup_printf ("-Dlog4j.configurationFile=%s", logging_file);
  g_assert_true (g_file_test (logging_file, G_FILE_TEST_EXISTS));
  g_assert_cmpstr (call.resolved->logging_argument, ==, path);

  /* The placeholders comfy knows are in, the ones it does not are not. */
  g_assert_cmpuint (call.resolved->jvm_arguments->len, ==, 3);
  g_free (path);
  path = g_strdup_printf ("-Djava.library.path=%s", natives_dir);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->jvm_arguments, 0), ==, path);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->jvm_arguments, 1), ==,
                   "-cp");
  g_assert_cmpstr (g_ptr_array_index (call.resolved->jvm_arguments, 2), ==,
                   classpath);

  g_assert_cmpuint (call.resolved->game_arguments->len, ==, 6);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 1), ==,
                   "${auth_player_name}");
  g_free (path);
  path = g_build_filename (data_dir, "assets", NULL);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 3), ==,
                   path);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 5), ==,
                   "test");

  /* Progress was reported and ended with the last file of the last
     stage. */
  g_assert_cmpuint (call.progress_calls, >, 0);
  g_assert_cmpstr (call.last_stage, ==, "assets");
  g_assert_cmpuint (call.last_done, ==, call.last_total);

  /* A second resolve asks the network for nothing. */
  g_clear_pointer (&call.resolved, comfy_resolved_version_free);
  g_clear_error (&call.error);
  fixture.hits = 0;

  start_resolve (&run, &call, call.manifest, "test-version", data_dir,
                 natives_dir, assets_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.resolved);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_cmpstr (call.resolved->classpath, ==, classpath);

  g_clear_object (&call.manifest);
  g_clear_pointer (&call.resolved, comfy_resolved_version_free);
  g_clear_error (&call.error);
  g_clear_pointer (&text, g_free);
  g_free (call.last_stage);
  free_served (&served);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a version is resolved, then served from the caches\n");
}

static void
test_resolve_legacy_assets (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *natives_dir = g_build_filename (dir, "instance", NULL);
  g_autofree char *assets_url = g_strdup (fixture.base);
  g_autofree char *copy = NULL;
  g_autofree char *path = NULL;
  g_autofree char *text = NULL;
  AsyncRun run = { 0 };
  ResolveCall call = { 0 };
  Served served;

  /* A version with an old index: virtual, and mapping its paths onto the
     resources of the game. */
  g_autofree char *manifest_url = server_url ("/manifest.json");
  fill_served (&served, "test-legacy", "legacy");
  served.virtual_index = TRUE;
  served.paths[0] = (char *) "sounds/step.ogg";
  served.paths[1] = NULL;
  serve_version (&served, dir);

  async_run_init (&run);
  fetch_manifest (&run, &call, manifest_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);

  start_resolve (&run, &call, call.manifest, "test-legacy", data_dir,
                 natives_dir, assets_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.resolved);

  /* The game is pointed at the virtual directory of the index and told
     no index name. */
  g_free (path);
  path = g_build_filename (data_dir, "assets", "virtual", "legacy", NULL);
  g_assert_cmpstr (call.resolved->assets_directory, ==, path);
  g_assert_null (call.resolved->assets_index_name);
  g_assert_cmpstr (g_ptr_array_index (call.resolved->game_arguments, 5), ==,
                   "legacy");

  /* Every object the index gives a path to was copied under it, with the
     prefix the game looks under. */
  copy = g_build_filename (data_dir, "assets", "virtual", "legacy",
                           "minecraft", "sounds", "step.ogg", NULL);
  g_free (text);
  text = read_text_file (copy);
  g_assert_cmpstr (text, ==, served.objects[0]->str);

  /* The object without a path was fetched but not copied. */
  {
    g_autofree char *bucket = g_strndup (served.hashes[1], 2);
    g_autofree char *object = g_build_filename (
        data_dir, "assets", "objects", bucket, served.hashes[1], NULL);

    g_assert_true (g_file_test (object, G_FILE_TEST_EXISTS));
  }

  g_clear_object (&call.manifest);
  g_clear_pointer (&call.resolved, comfy_resolved_version_free);
  g_clear_error (&call.error);
  g_clear_pointer (&text, g_free);
  g_free (call.last_stage);
  free_served (&served);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a virtual asset index is copied where the game reads it\n");
}

static void
test_resolve_failures (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *assets_url = g_strdup (fixture.base);
  AsyncRun run = { 0 };
  ResolveCall call = { 0 };
  Served served;

  g_autofree char *manifest_url = server_url ("/manifest.json");
  fill_served (&served, "test-version", "test");
  serve_version (&served, dir);

  async_run_init (&run);
  fetch_manifest (&run, &call, manifest_url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.manifest);

  /* A version the manifest does not know is not resolved. */
  start_resolve (&run, &call, call.manifest, "no-such-version", data_dir, NULL,
                 assets_url);
  async_run_wait (&run);

  g_assert_null (call.resolved);
  g_assert_error (call.error, comfy_MC_ERROR, comfy_MC_ERROR_NOT_FOUND);

  g_clear_error (&call.error);
  g_clear_object (&call.manifest);
  g_free (call.last_stage);
  free_served (&served);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a version that cannot be resolved fails as it should\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  cache_start ();
  fixture_start ();

  test_maven_paths ();
  test_entry_names ();
  test_native_classifiers ();
  test_classifier_is_not_natives ();
  test_rule_filtering ();
  test_classpath_order ();
  test_arguments ();
  test_natives_extraction ();
  test_default_exclude ();
  test_malicious_jar ();
  test_symlinked_directory ();
  test_resolve_modern ();
  test_resolve_legacy_assets ();
  test_resolve_failures ();

  fixture_stop ();
  cache_stop ();

  return EXIT_SUCCESS;
}
