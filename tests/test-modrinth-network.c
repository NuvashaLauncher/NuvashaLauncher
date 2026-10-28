#include "nuvashalauncher-modrinth-private.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-settings.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

typedef struct
{
  SoupServer *server;
  char *base;
  guint hits;
} NetworkFixture;

typedef struct _RequestWait RequestWait;

struct _RequestWait
{
  GMainLoop *loop;
  nuvashalauncherTaskScope *operation;
  GError *error;
  char *answer;
  RequestWait *group;
  guint pending;
};

static void
reply (SoupServerMessage *message, guint status, const char *body)
{
  soup_server_message_set_status (message, status, NULL);
  if (body != NULL)
    soup_server_message_set_response (message, "application/json", SOUP_MEMORY_COPY,
                                      body, strlen (body));
}

static void
server_handler (SoupServer *server G_GNUC_UNUSED, SoupServerMessage *message,
                const char *path, GHashTable *query G_GNUC_UNUSED,
                gpointer user_data)
{
  NetworkFixture *fixture = user_data;

  fixture->hits++;
  if (g_str_equal (path, "/ok"))
    reply (message, 200, "{\"kind\":\"fixture\",\"value\":42}");
  else if (g_str_equal (path, "/bad"))
    reply (message, 200, "not json");
  else if (g_str_equal (path, "/missing"))
    reply (message, 404, NULL);
  else
    reply (message, 500, NULL);
}

static void
answer_json (GTask *task, JsonNode *root)
{
  nuvashalauncher_modrinth_request_return_pointer (task, json_to_string (root, FALSE),
                                                   g_free);
}

static void
request_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  RequestWait *wait = user_data;

  wait->answer = g_task_propagate_pointer (G_TASK (result), &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  if (--wait->group->pending == 0)
    g_main_loop_quit (wait->group->loop);
}

static void
request_finished_two (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                      gpointer user_data)
{
  RequestWait *wait = user_data;
  g_autofree char *answer = NULL;

  answer = g_task_propagate_pointer (G_TASK (result), &wait->error);
  g_assert_nonnull (answer);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  if (--wait->group->pending == 0)
    g_main_loop_quit (wait->group->loop);
}

static char *
url_for (NetworkFixture *fixture, const char *path)
{
  return g_strconcat (fixture->base, path, NULL);
}

static void
test_success_and_deduplication (NetworkFixture *fixture)
{
  g_autofree char *url = url_for (fixture, "/ok?dedup=1");
  RequestWait first = { .loop = g_main_loop_new (NULL, FALSE) };
  RequestWait second = { .loop = first.loop, .group = &first };
  first.group = &first;
  first.pending = 2;

  fixture->hits = 0;
  first.operation = nuvashalauncher_modrinth_get_async (
    url, request_finished, &first, answer_json, nuvashalauncher_NOT_FOUND_IS_ERROR);
  second.operation = nuvashalauncher_modrinth_get_async (
    url, request_finished_two, &second, answer_json, nuvashalauncher_NOT_FOUND_IS_ERROR);
  g_main_loop_run (first.loop);

  g_assert_no_error (first.error);
  g_assert_cmpstr (first.answer, ==, "{\"kind\":\"fixture\",\"value\":42}");
  g_assert_no_error (second.error);
  g_assert_cmpuint (fixture->hits, ==, 1);
  g_free (first.answer);
  g_clear_error (&first.error);
  g_clear_error (&second.error);
  g_main_loop_unref (first.loop);

  g_print ("ok - Modrinth requests parse and deduplicate a successful flight\n");
}

static void
test_malformed_response (NetworkFixture *fixture)
{
  g_autofree char *url = url_for (fixture, "/bad?unique=1");
  RequestWait wait = { .loop = g_main_loop_new (NULL, FALSE) };

  wait.group = &wait;
  wait.pending = 1;

  wait.operation = nuvashalauncher_modrinth_get_async (
    url, request_finished, &wait, answer_json, nuvashalauncher_NOT_FOUND_IS_ERROR);
  g_main_loop_run (wait.loop);

  g_assert_null (wait.answer);
  g_assert_error (wait.error, nuvashalauncher_MODRINTH_ERROR,
                  nuvashalauncher_MODRINTH_ERROR_PARSE);
  g_clear_error (&wait.error);
  g_main_loop_unref (wait.loop);
  g_print ("ok - malformed Modrinth JSON is reported as a parse failure\n");
}

static void
test_not_found_and_cancellation (NetworkFixture *fixture)
{
  g_autofree char *missing = url_for (fixture, "/missing?unique=1");
  RequestWait absent = { .loop = g_main_loop_new (NULL, FALSE) };
  RequestWait cancelled = { .loop = g_main_loop_new (NULL, FALSE) };

  absent.group = &absent;
  absent.pending = 1;

  absent.operation = nuvashalauncher_modrinth_get_async (
    missing, request_finished, &absent, answer_json, nuvashalauncher_NOT_FOUND_IS_ABSENT);
  g_main_loop_run (absent.loop);
  g_assert_no_error (absent.error);
  g_assert_null (absent.answer);
  g_clear_error (&absent.error);
  g_main_loop_unref (absent.loop);

  {
    g_autoptr (GCancellable) cancellable = g_cancellable_new ();
    g_autofree char *url = url_for (fixture, "/ok?unique=cancel");

    cancelled.group = &cancelled;
    cancelled.pending = 1;
    cancelled.operation = nuvashalauncher_modrinth_get_async (
      url, request_finished, &cancelled, answer_json,
      nuvashalauncher_NOT_FOUND_IS_ERROR);
    nuvashalauncher_task_scope_follow_cancellable (cancelled.operation, cancellable);
    g_cancellable_cancel (cancellable);
    g_main_loop_run (cancelled.loop);
  }
  g_assert_null (cancelled.answer);
  g_assert_error (cancelled.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&cancelled.error);
  g_main_loop_unref (cancelled.loop);
  g_print ("ok - not-found and cancelled Modrinth requests finish cleanly\n");
}

int
main (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-modrinth-XXXXXX", &error);
  g_autofree char *config = g_build_filename (root, "config", NULL);
  g_autofree char *cache = g_build_filename (root, "cache", NULL);
  nuvashalauncherSettings *settings = NULL;
  NetworkFixture fixture = { 0 };
  GSList *uris = NULL;
  char *uri = NULL;

  g_assert_no_error (error);
  g_mkdir_with_parents (config, 0755);
  g_mkdir_with_parents (cache, 0755);
  g_setenv ("XDG_CONFIG_HOME", config, TRUE);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);
  settings = nuvashalauncher_settings_new ();
  nuvashalauncher_download_set_cache_settings (
    nuvashalauncher_downloads_get_service (), settings);

  fixture.server = soup_server_new ("server-header", "nuvashalauncher-test", NULL);
  g_assert_true (soup_server_listen_local (fixture.server, 0, 0, &error));
  soup_server_add_handler (fixture.server, "/", server_handler, &fixture, NULL);
  uris = soup_server_get_uris (fixture.server);
  uri = g_uri_to_string (uris->data);
  fixture.base = g_strndup (uri, strlen (uri) - 1);
  g_free (uri);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  test_success_and_deduplication (&fixture);
  test_malformed_response (&fixture);
  test_not_found_and_cancellation (&fixture);

  soup_server_disconnect (fixture.server);
  g_clear_object (&fixture.server);
  g_free (fixture.base);
  g_object_unref (settings);
  return 0;
}
