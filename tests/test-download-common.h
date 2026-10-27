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

/* ---------------------------------------------------------- *
 * Tests
 * ---------------------------------------------------------- */











































/* Nothing listens on port 1: the transport fails, gets retried and
 * finally surfaces as a network error of our own domain. */


#define MANY_DOWNLOADS 10





/* The shape a modpack fetch uses: nobody waits for the answer, the work is
 * tied to a cancel button it does not own, and the header row it reports
 * into is dropped while it still runs. The operation releases itself, so
 * the request state that borrows it must outlive the completion. */
