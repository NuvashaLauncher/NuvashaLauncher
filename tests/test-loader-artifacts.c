#define _GNU_SOURCE

#include "nuvashalauncher-loaders-private.h"
#include "nuvashalauncher-fs.h"

#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static char *
tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *path = g_dir_make_tmp ("nuvashalauncher-artifacts-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (path);
  return path;
}

static void
file_free (gpointer data)
{
  InstallFile *file = data;
  g_free (file->url);
  g_free (file->entry);
  g_free (file->path);
  g_free (file->sha1);
  g_free (file);
}

static InstallRequest
request_for (char *root, GPtrArray *pending)
{
  InstallRequest request = { 0 };
  request.loader = nuvashalauncher_LOADER_FORGE;
  request.root = root;
  request.libraries_dir = g_build_filename (root, "libraries", NULL);
  request.pending = pending;
  return request;
}

static void
test_artifact_queueing (void)
{
  g_autofree char *root = tmp_dir ();
  g_autofree char *existing = g_build_filename (
    root, "libraries", "net", "minecraftforge", "forge", "47.2.0",
    "forge-47.2.0.jar", NULL);
  g_autofree char *target = g_build_filename (root, "target.jar", NULL);
  g_autoptr (GPtrArray) pending = g_ptr_array_new_with_free_func (file_free);
  InstallRequest request = request_for (root, pending);

  g_autofree char *existing_dir = g_path_get_dirname (existing);
  g_assert_cmpint (g_mkdir_with_parents (existing_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (existing, "cached", -1, NULL));

  queue_artifact (&request, "net.minecraftforge:forge:47.2.0", FALSE);
  g_assert_cmpuint (pending->len, ==, 0);

  g_assert_cmpint (g_remove (existing), ==, 0);
  g_assert_cmpint (symlink (target, existing), ==, 0);
  {
    GStatBuf info;
    g_assert_cmpint (g_lstat (existing, &info), ==, 0);
    g_assert_true (S_ISLNK (info.st_mode));
  }
  queue_artifact (&request, "net.minecraftforge:forge:47.2.0", TRUE);
  g_assert_cmpuint (pending->len, ==, 1);
  {
    InstallFile *file = g_ptr_array_index (pending, 0);
    g_assert_cmpint (file->kind, ==, FILE_FETCH);
    g_assert_true (file->optional);
    g_assert_nonnull (strstr (file->url,
                             "/net/minecraftforge/forge/47.2.0/forge-47.2.0.jar"));
    g_assert_cmpstr (file->path, ==, existing);
  }

  queue_artifact (&request, "../../escape", FALSE);
  g_assert_cmpuint (pending->len, ==, 1);

  g_free (request.libraries_dir);
  nuvashalauncher_fs_remove_tree (root);
  g_print ("ok - cached artifacts are reused, symlinks are refetched, and unsafe coordinates are rejected\n");
}

static void
test_unpacked_queueing (void)
{
  g_autofree char *root = tmp_dir ();
  g_autofree char *existing = g_build_filename (root, "config", "a.cfg", NULL);
  g_autoptr (GPtrArray) pending = g_ptr_array_new_with_free_func (file_free);
  InstallRequest request = request_for (root, pending);

  queue_unpacked (&request, "/config/a.cfg");
  g_assert_cmpuint (pending->len, ==, 1);
  {
    InstallFile *file = g_ptr_array_index (pending, 0);
    g_assert_cmpint (file->kind, ==, FILE_UNPACK);
    g_assert_cmpstr (file->entry, ==, "config/a.cfg");
    g_assert_cmpstr (file->path, ==, existing);
    g_assert_false (file->optional);
  }

  queue_unpacked (&request, "../outside");
  queue_unpacked (&request, "");
  g_assert_cmpuint (pending->len, ==, 1);

  g_autofree char *existing_dir = g_path_get_dirname (existing);
  g_assert_cmpint (g_mkdir_with_parents (existing_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (existing, "already here", -1, NULL));
  queue_unpacked (&request, "/config/a.cfg");
  g_assert_cmpuint (pending->len, ==, 1);

  g_free (request.libraries_dir);
  nuvashalauncher_fs_remove_tree (root);
  g_print ("ok - unpacked entries normalize leading slashes and preserve existing files\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/loaders/artifact-queueing", test_artifact_queueing);
  g_test_add_func ("/loaders/unpacked-queueing", test_unpacked_queueing);
  return g_test_run ();
}
