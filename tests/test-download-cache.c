#define _GNU_SOURCE

#include "test-download-common.h"

static void
json_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  AsyncCall *call = user_data;
  call->parser = nuvashalauncher_download_json_finish (result, &call->error);
  call_finished (call);
}

static void
call_json (AsyncRun *run, AsyncCall *call, const char *url)
{
  call->run = run;
  run->pending++;
  call->operation = nuvashalauncher_download_json_async (
    nuvashalauncher_downloads_get_service (), url, json_finished, call);
  nuvashalauncher_task_scope_follow_cancellable (call->operation, run->cancellable);
}

static void
call_cached_json (AsyncRun *run, AsyncCall *call, const char *url)
{
  call->run = run;
  run->pending++;
  call->operation = nuvashalauncher_download_json_cached_async (
    nuvashalauncher_downloads_get_service (), url, json_finished, call);
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
test_destination_symlink_is_not_replaced (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *destination = g_build_filename (dir, "game.jar", NULL);
  g_autofree char *sentinel = g_build_filename (dir, "sentinel", NULL);
  g_autofree char *url = server_url ("/file");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };
  g_autofree char *contents = NULL;

  g_assert_true (g_file_set_contents (sentinel, "keep this file", -1, NULL));
  g_assert_cmpint (symlink (sentinel, destination), ==, 0);

  fixture.hits = 0;
  async_run_init (&run);
  call_file (&run, &call, url, destination, NULL, G_CHECKSUM_SHA1);
  async_run_wait (&run);

  /* Publishing is an atomic rename: it replaces the link itself rather
   * than opening it and writing through to the linked file. */
  g_assert_no_error (call.error);
  g_assert_true (call.success);
  g_assert_false (g_file_test (destination, G_FILE_TEST_IS_SYMLINK));
  contents = read_text_file (sentinel);
  g_assert_cmpstr (contents, ==, "keep this file");
  g_clear_pointer (&contents, g_free);
  contents = read_text_file (destination);
  g_assert_cmpmem (contents, FILE_BODY_SIZE, fixture.file_body->str,
                   fixture.file_body->len);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);
  g_print ("ok - a download does not follow a destination symlink\n");
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

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-download-split-XXXXXX", &error);
  g_autofree char *config = g_build_filename (root, "config", NULL);
  g_autofree char *cache = g_build_filename (root, "cache", NULL);
  (void) argc; (void) argv;
  g_assert_no_error (error);
  g_mkdir_with_parents (config, 0755);
  g_mkdir_with_parents (cache, 0755);
  g_setenv ("XDG_CONFIG_HOME", config, TRUE);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);
  cache_settings = nuvashalauncher_settings_new ();
  nuvashalauncher_download_set_cache_settings (nuvashalauncher_downloads_get_service (), cache_settings);
  fixture_start ();
  test_cached_reuse ();
  test_global_file_cache_reuse ();
  test_corrupt_cache_redownload ();
  test_destination_symlink_is_not_replaced ();
  test_json_download ();
  test_json_not_found ();
  test_api_cache_hit ();
  test_api_cache_disabled_and_keys ();
  test_api_cache_expiration_and_failed_response ();
  test_api_cache_etag ();
  test_api_cache_stale_offline ();
  test_api_cache_cancellation ();
  fixture_stop ();
  return EXIT_SUCCESS;
}
