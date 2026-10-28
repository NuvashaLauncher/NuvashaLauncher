#include "test-modpack-common.h"
#include "nuvashalauncher-loaders-private.h"

static guint
open_fd_count (void)
{
  g_autoptr (GError) error = NULL;
  GDir *directory = g_dir_open ("/proc/self/fd", 0, &error);
  guint count = 0;

  g_assert_no_error (error);
  g_assert_nonnull (directory);

  while (g_dir_read_name (directory) != NULL)
    count++;

  g_dir_close (directory);
  return count;
}

static void
install_file_free_for_test (gpointer data)
{
  InstallFile *file = data;

  g_free (file->url);
  g_free (file->entry);
  g_free (file->path);
  g_free (file->sha1);
  g_free (file);
}

static void
test_artifact_queue_does_not_trust_symlinks (void)
{
  g_autofree char *directory = make_tmp_dir ();
  g_autofree char *libraries = g_build_filename (directory, "libraries", NULL);
  g_autofree char *relative = g_strdup ("net/minecraftforge/forge/47.2.0/");
  g_autofree char *artifact = NULL;
  g_autofree char *target = g_build_filename (directory, "outside.jar", NULL);
  g_autofree char *artifact_dir = NULL;
  InstallRequest request = { 0 };

  request.loader = nuvashalauncher_LOADER_FORGE;
  request.libraries_dir = g_strdup (libraries);
  request.pending = g_ptr_array_new_with_free_func (install_file_free_for_test);
  artifact = g_build_filename (libraries, relative, "forge-47.2.0.jar", NULL);
  artifact_dir = g_path_get_dirname (artifact);

  /* A missing artifact is queued for download. */
  queue_artifact (&request, "net.minecraftforge:forge:47.2.0", FALSE);
  g_assert_cmpuint (request.pending->len, ==, 1);
  g_ptr_array_set_size (request.pending, 0);

  g_assert_cmpint (g_mkdir_with_parents (artifact_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (artifact, "jar", -1, NULL));

  /* A regular cached artifact is reused without another transfer. */
  queue_artifact (&request, "net.minecraftforge:forge:47.2.0", FALSE);
  g_assert_cmpuint (request.pending->len, ==, 0);

  /* A planted symlink must not be accepted as a cached classpath entry. */
  g_assert_true (g_file_set_contents (target, "outside", -1, NULL));
  g_assert_cmpint (g_remove (artifact), ==, 0);
  g_assert_cmpint (symlink (target, artifact), ==, 0);
  queue_artifact (&request, "net.minecraftforge:forge:47.2.0", FALSE);
  g_assert_cmpuint (request.pending->len, ==, 1);

  g_ptr_array_unref (request.pending);
  g_free (request.libraries_dir);
  remove_recursive (directory);
}

static void
test_archive_readers_close_fds (void)
{
  g_autofree char *directory = make_tmp_dir ();
  g_autofree char *archive = g_build_filename (directory, "archive.zip", NULL);
  g_autofree char *index = pack_index ("FD test", "", "\"minecraft\":\"1.20.1\"");
  g_autofree char *invalid = g_build_filename (directory, "invalid.zip", NULL);
  g_autofree char *truncated = g_build_filename (directory, "truncated.zip", NULL);
  g_autofree char *archive_data = NULL;
  gsize archive_size = 0;
  g_autoptr (GError) error = NULL;
  const ZipEntry entries[] = {
    { "modrinth.index.json", index, strlen (index), AE_IFREG, NULL },
    ZIP_FILE ("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\n\r\n"),
    ZIP_FILE ("install_profile.json", "{}"),
  };

  zip_write (archive, entries, G_N_ELEMENTS (entries));
  g_assert_true (g_file_set_contents (invalid, "not a zip", -1, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_get_contents (archive, &archive_data, &archive_size, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_set_contents (truncated, archive_data, archive_size / 2, &error));
  g_assert_no_error (error);

  guint before = open_fd_count ();
  for (guint i = 0; i < 32; i++)
    {
      g_autoptr (nuvashalauncherModpack) pack =
        nuvashalauncher_modpack_new_from_zip (archive, &error);
      g_assert_no_error (error);
      g_assert_nonnull (pack);
    }
  for (guint i = 0; i < 32; i++)
    {
      g_autoptr (GBytes) bytes =
        nuvashalauncher_loader_read_zip_entry (archive, "install_profile.json", &error);
      g_assert_no_error (error);
      g_assert_nonnull (bytes);
    }
  for (guint i = 0; i < 16; i++)
    {
      g_assert_true (nuvashalauncher_loader_write_jar_manifest_attribute (
                       archive, "Launcher-FD-Test", "ok", &error));
      g_assert_no_error (error);
    }
  g_assert_cmpuint (open_fd_count (), ==, before);

  /* Opening an existing non-archive exercises archive initialization failure. */
  for (guint i = 0; i < 16; i++)
    {
      g_autoptr (GBytes) bytes =
        nuvashalauncher_loader_read_zip_entry (invalid, "entry", &error);
      g_assert_null (bytes);
      g_clear_error (&error);
    }
  for (guint i = 0; i < 16; i++)
    {
      g_assert_false (nuvashalauncher_loader_write_jar_manifest_attribute (
                        invalid, "Launcher-FD-Test", "ok", &error));
      g_clear_error (&error);
    }
  /* The archive opens, but reading it fails after the output archive has
   * been initialized. This exercises both archive cleanup paths together. */
  for (guint i = 0; i < 16; i++)
    {
      g_assert_false (nuvashalauncher_loader_write_jar_manifest_attribute (
                        truncated, "Launcher-FD-Test", "ok", &error));
      g_clear_error (&error);
    }
  g_assert_cmpuint (open_fd_count (), ==, before);

  remove_recursive (directory);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/archive/file-descriptors-close", test_archive_readers_close_fds);
  g_test_add_func ("/loaders/artifact-queue-symlink", test_artifact_queue_does_not_trust_symlinks);
  return g_test_run ();
}
