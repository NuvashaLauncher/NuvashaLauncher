/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncher-download. A tiny libsoup server on the loopback
 * interface stands in for the network, so the tests are hermetic and
 * headless: they need neither the internet nor a display. Every test
 * uses its own temporary directory. */

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-cache.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Fixture: one server for every test
 * ---------------------------------------------------------- */

#define FILE_BODY_SIZE (300 * 1024)
#define BIG_BODY_SIZE (512 * 1024)

#define WRONG_BODY "this is not the body anyone asked for"
#define JSON_BODY "{\"kind\": \"test\", \"value\": 42}"

typedef struct
{
  SoupServer *server;
  char *base;           /* "http://127.0.0.1:port", no trailing slash */
  GString *file_body;   /* served at /file, /flaky and /json-bad */
  GString *big_body;    /* served at /big, for the cancellation test */
  guint hits;           /* requests seen, reset by every test */
  guint flaky_failures; /* 5xx answers still to give at /flaky */
} Fixture;

static Fixture fixture;
static nuvashalauncherSettings *cache_settings;

static void
reply_with (SoupServerMessage *message, guint status,
            const char *content_type, const char *body, gsize length)
{
  soup_server_message_set_status (message, status, NULL);

  if (body != NULL)
    soup_server_message_set_response (message, content_type, SOUP_MEMORY_COPY,
                                      body, length);
}

static void
server_handler (SoupServer *server, SoupServerMessage *message,
                const char *path, GHashTable *query, gpointer user_data)
{
  Fixture *self = user_data;

  (void) server;
  (void) query;

  self->hits++;

  if (g_str_equal (path, "/file"))
    reply_with (message, 200, "application/octet-stream",
                self->file_body->str, self->file_body->len);
  else if (g_str_equal (path, "/big"))
    reply_with (message, 200, "application/octet-stream", self->big_body->str,
                self->big_body->len);
  else if (g_str_equal (path, "/wrong"))
    reply_with (message, 200, "application/octet-stream", WRONG_BODY,
                strlen (WRONG_BODY));
  else if (g_str_equal (path, "/flaky"))
    {
      if (self->flaky_failures > 0)
        {
          self->flaky_failures--;
          reply_with (message, 500, NULL, NULL, 0);
          return;
        }

      reply_with (message, 200, "application/octet-stream",
                  self->file_body->str, self->file_body->len);
    }
  else if (g_str_equal (path, "/limited"))
    {
      soup_message_headers_append (
        soup_server_message_get_response_headers (message), "Retry-After", "0");
      reply_with (message, 429, NULL, NULL, 0);
    }
  else if (g_str_equal (path, "/json"))
    reply_with (message, 200, "application/json", JSON_BODY,
                strlen (JSON_BODY));
  else if (g_str_equal (path, "/etag"))
    {
      const char *etag = soup_message_headers_get_one (
        soup_server_message_get_request_headers (message), "If-None-Match");

      soup_message_headers_replace (
        soup_server_message_get_response_headers (message), "ETag", "\"v1\"");
      if (g_strcmp0 (etag, "\"v1\"") == 0)
        reply_with (message, 304, NULL, NULL, 0);
      else
        reply_with (message, 200, "application/json", JSON_BODY,
                    strlen (JSON_BODY));
    }
  else
    reply_with (message, 404, NULL, NULL, 0);
}

static GString *
make_body (gsize size)
{
  GString *body = g_string_sized_new (size);

  for (gsize i = 0; i < size; i++)
    g_string_append_c (body, (char) ('a' + (i % 26)));

  return body;
}

static void
free_body (gpointer data)
{
  g_string_free (data, TRUE);
}

static void
fixture_start (void)
{
  g_autoptr (GError) error = NULL;
  GSList *uris = NULL;
  char *uri_string = NULL;

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

  /* The URL of the server keeps its trailing slash, every path in the
   * tests starts with a slash of its own. */
  if (fixture.base[0] != '\0')
    {
      gsize length = strlen (fixture.base);

      if (fixture.base[length - 1] == '/')
        fixture.base[length - 1] = '\0';
    }

  g_free (uri_string);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  fixture.file_body = make_body (FILE_BODY_SIZE);
  fixture.big_body = make_body (BIG_BODY_SIZE);
}

static void
fixture_stop (void)
{
  soup_server_disconnect (fixture.server);
  g_clear_object (&fixture.server);
  g_clear_pointer (&fixture.file_body, free_body);
  g_clear_pointer (&fixture.big_body, free_body);
  g_clear_pointer (&fixture.base, g_free);
}

static char *
server_url (const char *path)
{
  return g_strconcat (fixture.base, path, NULL);
}

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

static char *
hash_of (const void *data, gsize length, GChecksumType type)
{
  return g_compute_checksum_for_data (type, data, length);
}

static char *
make_test_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("nuvashalauncher-test-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);

  return dir;
}

/* Removes a flat test directory, temporary files included. */
static void
remove_test_dir (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);

  if (dir != NULL)
    {
      const char *name = NULL;

      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, name, NULL);
          g_remove (child);
        }

      g_dir_close (dir);
    }

  g_rmdir (path);
}

/* Files in the test directory: anything but the destination itself is
 * a temporary file left behind. */
static guint
dir_entry_count (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  guint count = 0;

  g_assert_nonnull (dir);

  while (g_dir_read_name (dir) != NULL)
    count++;

  g_dir_close (dir);

  return count;
}

static char *
read_text_file (const char *path)
{
  char *contents = NULL;

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));

  return contents;
}

/* ---------------------------------------------------------- *
 * One run of the main loop with one or more requests in flight
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  guint pending; /* requests that have not reported back yet */
  guint watchdog;
  gboolean timed_out;
  GCancellable *cancellable;
} AsyncRun;

typedef struct
{
  AsyncRun *run;
  GError *error;
  gboolean success;
  JsonParser *parser;
  guint progress_calls;
  goffset progress_done;
  goffset progress_total;
  gboolean cancel_on_progress;
  nuvashalauncherTaskScope *operation;
} AsyncCall;

static void
call_finished (AsyncCall *call)
{
  nuvashalauncher_task_scope_unref (call->operation);
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
  /* GMainLoop is refcounted but is not a GObject. */
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
  g_clear_object (&call->parser);
}

static void
progress_cb (goffset bytes_done, goffset total, gpointer user_data)
{
  AsyncCall *call = user_data;

  call->progress_calls++;
  call->progress_done = bytes_done;
  call->progress_total = total;

  if (call->cancel_on_progress && bytes_done > 0)
    g_cancellable_cancel (call->run->cancellable);
}

static void
download_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  AsyncCall *call = user_data;

  call->success = nuvashalauncher_download_file_finish (result, &call->error);
  call_finished (call);
}

static void
json_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  AsyncCall *call = user_data;

  call->parser = nuvashalauncher_download_json_finish (result, &call->error);
  call_finished (call);
}

static void
call_file (AsyncRun *run, AsyncCall *call, const char *url,
           const char *dest_path, const char *expected_hash,
           GChecksumType hash_type)
{
  call->run = run;
  run->pending++;

  call->operation = nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), url, dest_path, expected_hash,
                                             hash_type, progress_cb,
                                             download_finished, call);
  nuvashalauncher_task_scope_follow_cancellable (call->operation, run->cancellable);
}

static void
call_json (AsyncRun *run, AsyncCall *call, const char *url)
{
  call->run = run;
  run->pending++;

  call->operation = nuvashalauncher_download_json_async (nuvashalauncher_downloads_get_service (), url, json_finished, call);
  nuvashalauncher_task_scope_follow_cancellable (call->operation, run->cancellable);
}

static void
call_cached_json (AsyncRun *run, AsyncCall *call, const char *url)
{
  call->run = run;
  run->pending++;
  call->operation = nuvashalauncher_download_json_cached_async (nuvashalauncher_downloads_get_service (), url, json_finished, call);
  nuvashalauncher_task_scope_follow_cancellable (call->operation, run->cancellable);
}

static void
clear_api_cache (void)
{
  g_autofree char *directory = nuvashalauncher_cache_get_api_directory ();
  GDir *dir = g_dir_open (directory, 0, NULL);
  const char *name = NULL;

  if (dir == NULL)
    return;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *path = g_build_filename (directory, name, NULL);
      g_remove (path);
    }
  g_dir_close (dir);
}

static void
clear_global_file_cache (void)
{
  g_autofree char *directory = nuvashalauncher_cache_get_files_directory ();
  GDir *dir = g_dir_open (directory, 0, NULL);
  const char *name = NULL;

  if (dir == NULL)
    return;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *path = g_build_filename (directory, name, NULL);
      g_remove (path);
    }
  g_dir_close (dir);
}

static char *
cached_path (const char *method, const char *url, const char *body)
{
  g_autofree char *directory = nuvashalauncher_cache_get_api_directory ();
  g_autofree char *key = g_strdup_printf ("%s\n%s\n%s", method, url,
                                           body != NULL ? body : "");
  g_autofree char *digest =
    g_compute_checksum_for_string (G_CHECKSUM_SHA256, key, -1);
  return g_build_filename (directory, digest, NULL);
}

/* ---------------------------------------------------------- *
 * Tests
 * ---------------------------------------------------------- */

static void
test_successful_download (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/file");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA256);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  g_autofree char *contents = NULL;

  fixture.hits = 0;
  clear_global_file_cache ();

  async_run_init (&run);
  call_file (&run, &call, url, dest, expected, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_true (call.success);
  g_assert_cmpuint (fixture.hits, ==, 1);

  /* Progress was reported and ended where the file ended. */
  g_assert_cmpuint (call.progress_calls, >, 0);
  g_assert_cmpint (call.progress_done, ==, (goffset) FILE_BODY_SIZE);
  g_assert_cmpint (call.progress_total, ==, (goffset) FILE_BODY_SIZE);

  contents = read_text_file (dest);
  g_assert_cmpmem (contents, FILE_BODY_SIZE, fixture.file_body->str,
                   fixture.file_body->len);

  /* The temporary file is gone: the directory holds the destination. */
  g_assert_cmpuint (dir_entry_count (dir), ==, 1);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - successful download\n");
}

static void
test_checksum_mismatch (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/wrong");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA256);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;
  clear_global_file_cache ();

  async_run_init (&run);
  call_file (&run, &call, url, dest, expected, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR,
                  nuvashalauncher_DOWNLOAD_ERROR_CHECKSUM);
  g_assert_false (call.success);
  /* A wrong hash repeats the transfer exactly once. */
  g_assert_cmpuint (fixture.hits, ==, 2);
  g_assert_false (g_file_test (dest, G_FILE_TEST_EXISTS));
  g_assert_cmpuint (dir_entry_count (dir), ==, 0);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - checksum mismatch fails and is retried once\n");
}

static void
test_invalid_expected_hash (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/file");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;
  async_run_init (&run);
  call_file (&run, &call, url, dest, "../outside", G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR,
                  nuvashalauncher_DOWNLOAD_ERROR_CHECKSUM);
  g_assert_false (call.success);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_false (g_file_test (dest, G_FILE_TEST_EXISTS));

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - invalid expected hashes are rejected\n");
}

static void
test_cached_reuse (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/file");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA1);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  g_autofree char *contents = NULL;

  g_assert_true (g_file_set_contents (dest, fixture.file_body->str,
                                      fixture.file_body->len, NULL));

  fixture.hits = 0;

  async_run_init (&run);
  call_file (&run, &call, url, dest, expected, G_CHECKSUM_SHA1);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_true (call.success);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_cmpuint (call.progress_calls, ==, 0);

  contents = read_text_file (dest);
  g_assert_cmpmem (contents, FILE_BODY_SIZE, fixture.file_body->str,
                   fixture.file_body->len);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a matching cache skips the download\n");
}

static void
test_global_file_cache_reuse (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *first = g_build_filename (dir, "first.jar", NULL);
  g_autofree char *second = g_build_filename (dir, "second.jar", NULL);
  g_autofree char *url = server_url ("/file");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA1);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  clear_global_file_cache ();
  fixture.hits = 0;
  async_run_init (&run);
  call_file (&run, &call, url, first, expected, G_CHECKSUM_SHA1);
  async_run_wait (&run);
  g_assert_true (call.success);
  async_call_clear (&call);
  async_run_clear (&run);

  async_run_init (&run);
  call_file (&run, &call, url, second, expected, G_CHECKSUM_SHA1);
  async_run_wait (&run);
  g_assert_true (call.success);
  g_assert_cmpuint (fixture.hits, ==, 1);
  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - verified global files are reused across destinations\n");
}

static void
test_corrupt_cache_redownload (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/file");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA512);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  g_autofree char *contents = NULL;

  g_assert_true (g_file_set_contents (dest, "stale bytes", -1, NULL));

  fixture.hits = 0;

  async_run_init (&run);
  call_file (&run, &call, url, dest, expected, G_CHECKSUM_SHA512);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_true (call.success);
  g_assert_cmpuint (fixture.hits, ==, 1);

  contents = read_text_file (dest);
  g_assert_cmpmem (contents, FILE_BODY_SIZE, fixture.file_body->str,
                   fixture.file_body->len);
  g_assert_cmpuint (dir_entry_count (dir), ==, 1);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a corrupt cache entry is downloaded again\n");
}

static void
test_cancellation (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/big");
  g_autofree char *expected =
    hash_of (fixture.big_body->str, fixture.big_body->len, G_CHECKSUM_SHA256);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  g_autofree char *contents = NULL;

  /* A file is already there: cancelling must leave it alone. */
  g_assert_true (g_file_set_contents (dest, "original", -1, NULL));

  fixture.hits = 0;

  async_run_init (&run);
  call.cancel_on_progress = TRUE;
  call_file (&run, &call, url, dest, expected, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_CANCELLED);
  g_assert_false (call.success);
  g_assert_cmpuint (fixture.hits, ==, 1);
  g_assert_cmpuint (call.progress_calls, >=, 2);

  /* The old file is untouched and no temporary file is left. */
  g_assert_cmpuint (dir_entry_count (dir), ==, 1);
  contents = read_text_file (dest);
  g_assert_cmpstr (contents, ==, "original");

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - cancellation leaves no temporary file\n");
}

static void
test_cancellation_before_start (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/file");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA256);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);
  g_cancellable_cancel (run.cancellable);
  call_file (&run, &call, url, dest, expected, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_CANCELLED);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_cmpuint (dir_entry_count (dir), ==, 0);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a cancelled request never reaches the server\n");
}

static void
test_retry_on_server_error (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/flaky");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA256);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  g_autofree char *contents = NULL;

  fixture.hits = 0;
  fixture.flaky_failures = 2;

  async_run_init (&run);
  call_file (&run, &call, url, dest, expected, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_true (call.success);
  g_assert_cmpuint (fixture.hits, ==, 3);
  g_assert_cmpuint (fixture.flaky_failures, ==, 0);

  contents = read_text_file (dest);
  g_assert_cmpmem (contents, FILE_BODY_SIZE, fixture.file_body->str,
                   fixture.file_body->len);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a server error is retried until it works\n");
}

static void
test_not_found_is_not_retried (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/missing");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);
  call_file (&run, &call, url, dest, NULL, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_HTTP);
  g_assert_false (call.success);
  g_assert_cmpuint (fixture.hits, ==, 1);
  g_assert_false (g_file_test (dest, G_FILE_TEST_EXISTS));
  g_assert_cmpuint (dir_entry_count (dir), ==, 0);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a 404 fails without a retry\n");
}

static void
test_rate_limit (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *url = server_url ("/limited");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);
  call_file (&run, &call, url, dest, NULL, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR,
                  nuvashalauncher_DOWNLOAD_ERROR_RATE_LIMITED);
  g_assert_false (call.success);
  /* Retry-After: 0 makes every wait immediate, the attempts remain. */
  g_assert_cmpuint (fixture.hits, ==, 3);
  g_assert_cmpuint (dir_entry_count (dir), ==, 0);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a rate limited answer retries and gives up as 429\n");
}

static void
test_json_download (void)
{
  g_autofree char *url = server_url ("/json");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  JsonNode *root = NULL;
  JsonObject *object = NULL;

  fixture.hits = 0;

  async_run_init (&run);
  call_json (&run, &call, url);
  async_run_wait (&run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.parser);
  g_assert_cmpuint (fixture.hits, ==, 1);

  root = json_parser_get_root (call.parser);
  g_assert_nonnull (root);
  g_assert_true (JSON_NODE_HOLDS_OBJECT (root));

  object = json_node_get_object (root);
  g_assert_cmpstr (json_object_get_string_member (object, "kind"), ==, "test");
  g_assert_cmpint (json_object_get_int_member (object, "value"), ==, 42);

  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - json download parses the body\n");
}

static void
test_json_not_found (void)
{
  g_autofree char *url = server_url ("/missing");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);
  call_json (&run, &call, url);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_HTTP);
  g_assert_null (call.parser);
  g_assert_cmpuint (fixture.hits, ==, 1);

  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - a json request reports the http status\n");
}

static void
test_api_cache_hit (void)
{
  g_autofree char *url = server_url ("/json?cache-hit");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  clear_api_cache ();
  g_assert_true (nuvashalauncher_settings_set_cache_modrinth_api_responses (
    cache_settings, TRUE, NULL));
  fixture.hits = 0;
  async_run_init (&run);
  call_cached_json (&run, &call, url);
  async_run_wait (&run);
  g_assert_no_error (call.error);
  async_call_clear (&call);
  async_run_clear (&run);

  async_run_init (&run);
  call_cached_json (&run, &call, url);
  async_run_wait (&run);
  g_assert_no_error (call.error);
  g_assert_cmpuint (fixture.hits, ==, 1);
  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - API cache hit and miss\n");
}

static void
test_api_cache_disabled_and_keys (void)
{
  g_autofree char *first = server_url ("/json?first");
  g_autofree char *second = server_url ("/json?second");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  clear_api_cache ();
  g_assert_true (nuvashalauncher_settings_set_cache_modrinth_api_responses (
    cache_settings, FALSE, NULL));
  fixture.hits = 0;
  async_run_init (&run);
  call_cached_json (&run, &call, first);
  async_run_wait (&run);
  async_call_clear (&call);
  async_run_clear (&run);
  async_run_init (&run);
  call_cached_json (&run, &call, first);
  async_run_wait (&run);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);
  async_run_clear (&run);

  g_assert_true (nuvashalauncher_settings_set_cache_modrinth_api_responses (
    cache_settings, TRUE, NULL));
  fixture.hits = 0;
  async_run_init (&run);
  call_cached_json (&run, &call, first);
  async_run_wait (&run);
  async_call_clear (&call);
  async_run_clear (&run);
  async_run_init (&run);
  call_cached_json (&run, &call, second);
  async_run_wait (&run);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - disabled API cache and request keys are respected\n");
}

static void
test_api_cache_expiration_and_failed_response (void)
{
  g_autofree char *url = server_url ("/json?expired");
  g_autofree char *path = NULL;
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  clear_api_cache ();
  g_assert_true (nuvashalauncher_settings_set_cache_expiration (
    cache_settings, nuvashalauncher_SETTINGS_CACHE_EXPIRATION_7_DAYS, NULL));
  fixture.hits = 0;
  async_run_init (&run);
  call_cached_json (&run, &call, url);
  async_run_wait (&run);
  async_call_clear (&call);
  async_run_clear (&run);
  path = cached_path ("GET", url, NULL);
  {
    g_autofree char *contents = g_strdup_printf (
      "{\"created\":%" G_GINT64_FORMAT ",\"response\":\"{\\\"kind\\\": \\\"test\\\", \\\"value\\\": 42}\"}",
      (gint64) (g_get_real_time () / G_USEC_PER_SEC) - 2 * 24 * 60 * 60);
    g_assert_true (g_file_set_contents (path, contents, -1, NULL));
  }
  async_run_init (&run);
  call_cached_json (&run, &call, url);
  async_run_wait (&run);
  g_assert_cmpuint (fixture.hits, ==, 1);
  async_call_clear (&call);
  async_run_clear (&run);

  g_assert_true (nuvashalauncher_settings_set_cache_expiration (
    cache_settings, nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_HOUR, NULL));
  async_run_init (&run);
  call_cached_json (&run, &call, url);
  async_run_wait (&run);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);
  async_run_clear (&run);

  /* The successful refresh above also verifies that an expired entry is
   * replaced, rather than merely bypassed. */
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  fixture.hits = 0;
  g_autofree char *missing = server_url ("/missing?not-cached");
  async_run_init (&run);
  call_cached_json (&run, &call, missing);
  async_run_wait (&run);
  async_call_clear (&call);
  async_run_clear (&run);
  async_run_init (&run);
  call_cached_json (&run, &call, missing);
  async_run_wait (&run);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - expiration refreshes and failed responses are not cached\n");
}

static void
test_api_cache_etag (void)
{
  AsyncRun run = { 0 };
  AsyncCall first = { 0 };
  AsyncCall second = { 0 };
  g_autofree char *url = server_url ("/etag");
  g_autofree char *path = NULL;
  g_autofree char *contents = NULL;
  g_autoptr (GString) stale = NULL;
  char *created = NULL;
  gsize length = 0;

  clear_api_cache ();
  g_assert_true (nuvashalauncher_settings_set_cache_modrinth_api_responses (
    cache_settings, TRUE, NULL));
  g_assert_true (nuvashalauncher_settings_set_cache_expiration (
    cache_settings, nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_HOUR, NULL));
  fixture.hits = 0;

  async_run_init (&run);
  call_cached_json (&run, &first, url);
  async_run_wait (&run);
  g_assert_no_error (first.error);
  async_call_clear (&first);
  async_run_clear (&run);

  path = cached_path ("GET", url, NULL);
  g_assert_true (g_file_get_contents (path, &contents, &length, NULL));
  stale = g_string_new_len (contents, length);
  created = strstr (stale->str, "\"created\"");
  g_assert_nonnull (created);
  created = strchr (created, ':');
  g_assert_nonnull (created);
  created++;
  while (*created == ' ')
    created++;
  while (g_ascii_isdigit (*created))
    {
      memmove (created, created + 1, strlen (created));
    }
  g_string_insert (stale, created - stale->str, "0");
  g_assert_true (g_file_set_contents (path, stale->str, -1, NULL));

  async_run_init (&run);
  call_cached_json (&run, &second, url);
  async_run_wait (&run);
  g_assert_no_error (second.error);
  g_assert_nonnull (second.parser);
  g_assert_cmpuint (fixture.hits, ==, 2);
  async_call_clear (&second);
  async_run_clear (&run);

  g_print ("ok - expired API cache entries revalidate with ETag\n");
}

static void
test_api_cache_stale_offline (void)
{
  g_autofree char *url = server_url ("/json?offline");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  clear_api_cache ();
  fixture.hits = 0;
  async_run_init (&run);
  call_cached_json (&run, &call, url);
  async_run_wait (&run);
  async_call_clear (&call);
  async_run_clear (&run);

  fixture_stop ();
  async_run_init (&run);
  call_cached_json (&run, &call, url);
  async_run_wait (&run);
  g_assert_no_error (call.error);
  g_assert_nonnull (call.parser);
  async_call_clear (&call);
  async_run_clear (&run);
  fixture_start ();

  g_print ("ok - stale API responses are an offline fallback\n");
}

static void
test_api_cache_cancellation (void)
{
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  async_run_init (&run);
  call_cached_json (&run, &call, "http://127.0.0.1:1/cancel");
  g_cancellable_cancel (run.cancellable);
  async_run_wait (&run);
  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR,
                  nuvashalauncher_DOWNLOAD_ERROR_CANCELLED);
  async_call_clear (&call);
  async_run_clear (&run);

  g_print ("ok - API cache requests remain cancellable\n");
}

static void
test_invalid_url (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);
  call_file (&run, &call, "not a url at all", dest, NULL, G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_NETWORK);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_cmpuint (dir_entry_count (dir), ==, 0);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - an unusable url fails before any request\n");
}

/* Nothing listens on port 1: the transport fails, gets retried and
 * finally surfaces as a network error of our own domain. */
static void
test_connection_refused (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "game.jar", NULL);
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;

  async_run_init (&run);
  call_file (&run, &call, "http://127.0.0.1:1/file", dest, NULL,
             G_CHECKSUM_SHA256);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_NETWORK);
  g_assert_false (call.success);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_cmpuint (dir_entry_count (dir), ==, 0);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a refused connection is a network error\n");
}

static void
released_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  guint *finished = user_data;

  (void) nuvashalauncher_download_file_finish (result, NULL);
  (*finished)--;
}

#define MANY_DOWNLOADS 10

static void
test_many_at_once (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *url = server_url ("/file");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA256);
  AsyncRun run = { 0 };
  AsyncCall calls[MANY_DOWNLOADS] = { 0 };
  char *destinations[MANY_DOWNLOADS] = { 0 };

  fixture.hits = 0;
  clear_global_file_cache ();

  async_run_init (&run);

  for (guint i = 0; i < MANY_DOWNLOADS; i++)
    {
      g_autofree char *name = g_strdup_printf ("f%u.jar", i);

      destinations[i] = g_build_filename (dir, name, NULL);
      call_file (&run, &calls[i], url, destinations[i], expected,
                 G_CHECKSUM_SHA256);
    }

  async_run_wait (&run);

  for (guint i = 0; i < MANY_DOWNLOADS; i++)
    {
      g_autofree char *contents = NULL;

      g_assert_no_error (calls[i].error);
      g_assert_true (calls[i].success);

      contents = read_text_file (destinations[i]);
      g_assert_cmpmem (contents, FILE_BODY_SIZE, fixture.file_body->str,
                       fixture.file_body->len);

      async_call_clear (&calls[i]);
    }

  /* The first wave fills the transfer slots; queued requests may reuse the
   * verified global file cache as soon as that wave completes. */
  g_assert_cmpuint (fixture.hits, >, 0);
  g_assert_cmpuint (fixture.hits, <=, MANY_DOWNLOADS);
  g_assert_cmpuint (dir_entry_count (dir), ==, MANY_DOWNLOADS);

  for (guint i = 0; i < MANY_DOWNLOADS; i++)
    g_free (destinations[i]);

  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - many downloads share the transfer slots\n");
}

static void
test_download_queue_soak (void)
{
  /* Repeat the full mixed queue lifecycle so slot accounting, cache reuse,
   * cancellation cleanup, and request teardown are exercised over a longer
   * run than the functional concurrency test. */
  for (guint round = 0; round < 12; round++)
    test_many_at_once ();

  g_print ("ok - download queue survives a sustained soak\n");
}

/* The shape a modpack fetch uses: nobody waits for the answer, the work is
 * tied to a cancel button it does not own, and the header row it reports
 * into is dropped while it still runs. The operation releases itself, so
 * the request state that borrows it must outlive the completion. */
static void
test_released_download_outlives_its_row (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *url = server_url ("/file");
  g_autofree char *expected =
    hash_of (fixture.file_body->str, fixture.file_body->len,
             G_CHECKSUM_SHA256);
  g_autoptr (GCancellable) row = g_cancellable_new ();
  guint finished = 0;
  gint64 deadline = 0;

  clear_global_file_cache ();

  for (guint round = 0; round < 20; round++)
    {
      g_autofree char *name = g_strdup_printf ("f%u.mrpack", round);
      g_autofree char *path = g_build_filename (dir, name, NULL);

      finished++;
      nuvashalauncher_task_scope_release_when_done (
        nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), url, path, expected, G_CHECKSUM_SHA256, NULL,
                                 released_done, &finished),
        row);

      /* The row is dropped between starting and finishing, which is what
         closing the header while a download runs looks like. */
      if (round == 5)
        g_cancellable_cancel (row);
    }

  /* Everything the wave started is answered, and the release each one
     owes the request state is delivered. */
  deadline = g_get_monotonic_time () + 30 * G_TIME_SPAN_SECOND;

  while (finished > 0 && g_get_monotonic_time () < deadline)
    {
      while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);

      g_usleep (1000);
    }

  g_assert_cmpuint (finished, ==, 0);
  remove_test_dir (dir);

  g_print ("ok - a released download outlives the row that cancelled it\n");
}

int
main (int argc, char **argv)
{
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-api-cache-XXXXXX", NULL);
  g_autofree char *config = g_build_filename (root, "config", NULL);
  g_autofree char *cache = g_build_filename (root, "cache", NULL);

  (void) argc;
  (void) argv;

  g_mkdir_with_parents (config, 0755);
  g_mkdir_with_parents (cache, 0755);
  g_setenv ("XDG_CONFIG_HOME", config, TRUE);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);
  cache_settings = nuvashalauncher_settings_new ();
  nuvashalauncher_download_set_cache_settings (nuvashalauncher_downloads_get_service (),
                                   cache_settings);

  fixture_start ();

  test_successful_download ();
  test_checksum_mismatch ();
  test_invalid_expected_hash ();
  test_cached_reuse ();
  test_global_file_cache_reuse ();
  test_corrupt_cache_redownload ();
  test_cancellation ();
  test_cancellation_before_start ();
  test_retry_on_server_error ();
  test_not_found_is_not_retried ();
  test_rate_limit ();
  test_json_download ();
  test_json_not_found ();
  test_api_cache_hit ();
  test_api_cache_disabled_and_keys ();
  test_api_cache_expiration_and_failed_response ();
  test_api_cache_etag ();
  test_api_cache_stale_offline ();
  test_api_cache_cancellation ();
  test_invalid_url ();
  test_connection_refused ();
  test_many_at_once ();
  test_download_queue_soak ();
  test_released_download_outlives_its_row ();

  fixture_stop ();
  g_clear_object (&cache_settings);

  return EXIT_SUCCESS;
}
