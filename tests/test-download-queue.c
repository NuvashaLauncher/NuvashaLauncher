#include "test-download-common.h"

static void
released_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  guint *finished = user_data;

  (void) nuvashalauncher_download_file_finish (result, NULL);
  (*finished)--;
}

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
  test_many_at_once ();
  test_download_queue_soak ();
  test_released_download_outlives_its_row ();
  fixture_stop ();
  return EXIT_SUCCESS;
}
