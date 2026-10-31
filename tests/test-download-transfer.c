#include "test-download-common.h"

static void
call_file_limited (AsyncRun *run, AsyncCall *call, const char *url,
                   const char *dest_path, goffset max_size)
{
  call->run = run;
  run->pending++;
  call->operation = nuvashalauncher_download_file_limited_async (
    nuvashalauncher_downloads_get_service (), url, dest_path, NULL,
    G_CHECKSUM_SHA256, max_size, progress_cb, download_finished, call);
  nuvashalauncher_task_scope_follow_cancellable (call->operation,
                                                 run->cancellable);
}

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
test_file_size_limit (void)
{
  g_autofree char *dir = make_test_dir ();
  g_autofree char *dest = g_build_filename (dir, "limited.jar", NULL);
  g_autofree char *url = server_url ("/file");
  AsyncRun run = { 0 };
  AsyncCall call = { 0 };

  fixture.hits = 0;
  async_run_init (&run);
  call_file_limited (&run, &call, url, dest, FILE_BODY_SIZE - 1);
  async_run_wait (&run);

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR,
                  nuvashalauncher_DOWNLOAD_ERROR_INVALID);
  g_assert_false (call.success);
  g_assert_cmpuint (fixture.hits, ==, 1);
  g_assert_false (g_file_test (dest, G_FILE_TEST_EXISTS));

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - a file download obeys its caller-supplied size limit\n");
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
                  nuvashalauncher_DOWNLOAD_ERROR_INVALID);
  g_assert_false (call.success);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_false (g_file_test (dest, G_FILE_TEST_EXISTS));

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - invalid expected hashes are rejected\n");
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

  g_assert_error (call.error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_INVALID);
  g_assert_cmpuint (fixture.hits, ==, 0);
  g_assert_cmpuint (dir_entry_count (dir), ==, 0);

  async_call_clear (&call);
  async_run_clear (&run);
  remove_test_dir (dir);

  g_print ("ok - an unusable url fails before any request\n");
}

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
  test_successful_download ();
  test_file_size_limit ();
  test_checksum_mismatch ();
  test_invalid_expected_hash ();
  test_cancellation ();
  test_cancellation_before_start ();
  test_retry_on_server_error ();
  test_not_found_is_not_retried ();
  test_rate_limit ();
  test_invalid_url ();
  test_connection_refused ();
  fixture_stop ();
  return EXIT_SUCCESS;
}
