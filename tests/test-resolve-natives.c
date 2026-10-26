#include "test-resolve-common.h"

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
    { "/tmp/nuvashalauncher-resolve-escaped.so", "outside", FALSE },
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
  g_assert_error (call.error, nuvashalauncher_RESOLVE_ERROR,
                  nuvashalauncher_RESOLVE_ERROR_UNSAFE_PATH);
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
  g_assert_error (call.error, nuvashalauncher_RESOLVE_ERROR,
                  nuvashalauncher_RESOLVE_ERROR_UNSAFE_PATH);
  g_assert_false (g_file_test ("/tmp/nuvashalauncher-resolve-escaped.so",
                               G_FILE_TEST_EXISTS));
  g_clear_error (&call.error);

  write_jar (jar, nested, G_N_ELEMENTS (nested));
  async_run_init (&run);
  extract_one (jar, natives, exclude, &call);
  async_run_wait (&run);
  async_run_clear (&run);

  g_assert_null (call.success);
  g_assert_error (call.error, nuvashalauncher_RESOLVE_ERROR,
                  nuvashalauncher_RESOLVE_ERROR_UNSAFE_PATH);
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

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  cache_start ();
  fixture_start ();
  test_natives_extraction ();
  test_default_exclude ();
  test_malicious_jar ();
  test_symlinked_directory ();
  fixture_stop ();
  cache_stop ();
  return EXIT_SUCCESS;
}
