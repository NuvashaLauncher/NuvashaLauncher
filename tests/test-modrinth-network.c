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
  NuvashaLauncherTaskScope *operation;
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
  else if (g_str_equal (path, "/version_files/update"))
    {
      SoupMessageBody *req_body = soup_server_message_get_request_body (message);
      const char *method = soup_server_message_get_method (message);

      g_assert_cmpstr (method, ==, "POST");
      g_assert_nonnull (req_body);
      g_assert_nonnull (req_body->data);
      g_assert_nonnull (strstr (req_body->data, "\"hash1\""));
      g_assert_nonnull (strstr (req_body->data, "\"hash2\""));
      g_assert_nonnull (strstr (req_body->data, "\"algorithm\":\"sha1\""));
      g_assert_nonnull (strstr (req_body->data, "\"loaders\":[\"fabric\"]"));
      g_assert_nonnull (strstr (req_body->data, "\"game_versions\":[\"1.20.1\"]"));

      reply (message, 200,
             "{"
             "\"hash1\":{"
               "\"id\":\"v2\","
               "\"project_id\":\"proj1\","
               "\"name\":\"Update 2\","
               "\"version_number\":\"2.0.0\","
               "\"game_versions\":[\"1.20.1\"],"
               "\"loaders\":[\"fabric\"],"
               "\"files\":[{\"url\":\"https://example.invalid/mod2.jar\",\"filename\":\"mod2.jar\",\"primary\":true,\"hashes\":{\"sha1\":\"newsha1\"}}]"
             "},"
             "\"hash2\":{"
               "\"id\":\"v3\","
               "\"project_id\":\"proj2\","
               "\"name\":\"Wrong Loader\","
               "\"version_number\":\"3.0.0\","
               "\"game_versions\":[\"1.20.1\"],"
               "\"loaders\":[\"forge\"],"
               "\"files\":[{\"url\":\"https://example.invalid/mod3.jar\",\"filename\":\"mod3.jar\",\"primary\":true,\"hashes\":{\"sha1\":\"newsha2\"}}]"
             "}"
             "}");
    }
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

typedef struct
{
  GMainLoop *loop;
  NuvashaLauncherTaskScope *operation;
  GHashTable *updates;
  GError *error;
} BatchUpdatesWait;

static void
batch_updates_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                        gpointer user_data)
{
  BatchUpdatesWait *wait = user_data;

  wait->updates = nuvashalauncher_modrinth_updates_by_hashes_finish (result, &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

static void
test_batch_updates_network (NetworkFixture *fixture)
{
  g_autofree char *url = url_for (fixture, "/version_files/update");
  g_autoptr (GPtrArray) hashes = g_ptr_array_new ();
  g_autoptr (GPtrArray) empty_hashes = g_ptr_array_new ();
  BatchUpdatesWait wait = { 0 };
  BatchUpdatesWait empty_wait = { 0 };
  NuvashaLauncherModrinthVersion *v1 = NULL;
  guint hits_before = fixture->hits;

  g_ptr_array_add (hashes, "hash1");
  g_ptr_array_add (hashes, "hash2");

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.operation = nuvashalauncher_modrinth_updates_by_hashes_url_async (
    url, nuvashalauncher_CONTENT_MOD, hashes, "1.20.1", "fabric",
    batch_updates_finished, &wait);
  g_main_loop_run (wait.loop);

  g_assert_no_error (wait.error);
  g_assert_nonnull (wait.updates);
  g_assert_cmpuint (fixture->hits, ==, hits_before + 1);
  g_assert_cmpuint (g_hash_table_size (wait.updates), ==, 1);

  v1 = g_hash_table_lookup (wait.updates, "hash1");
  g_assert_nonnull (v1);
  g_assert_cmpstr (v1->version_id, ==, "v2");
  g_assert_null (g_hash_table_lookup (wait.updates, "hash2"));

  g_hash_table_unref (wait.updates);
  g_main_loop_unref (wait.loop);

  hits_before = fixture->hits;
  empty_wait.loop = g_main_loop_new (NULL, FALSE);
  empty_wait.operation = nuvashalauncher_modrinth_updates_by_hashes_url_async (
    url, nuvashalauncher_CONTENT_MOD, empty_hashes, "1.20.1", "fabric",
    batch_updates_finished, &empty_wait);
  g_main_loop_run (empty_wait.loop);

  g_assert_no_error (empty_wait.error);
  g_assert_nonnull (empty_wait.updates);
  g_assert_cmpuint (g_hash_table_size (empty_wait.updates), ==, 0);
  g_assert_cmpuint (fixture->hits, ==, hits_before);

  g_hash_table_unref (empty_wait.updates);
  g_main_loop_unref (empty_wait.loop);

  g_print ("ok - batch Modrinth updates send one POST and filter incompatible versions\n");
}

int
main (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-modrinth-XXXXXX", &error);
  g_autofree char *config = g_build_filename (root, "config", NULL);
  g_autofree char *cache = g_build_filename (root, "cache", NULL);
  NuvashaLauncherSettings *settings = NULL;
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
  test_batch_updates_network (&fixture);

  soup_server_disconnect (fixture.server);
  g_clear_object (&fixture.server);
  g_free (fixture.base);
  g_object_unref (settings);
  return 0;
}
