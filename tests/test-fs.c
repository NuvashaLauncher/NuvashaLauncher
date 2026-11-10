#define _GNU_SOURCE

/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-fs.h"

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

int
main (void)
{
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-fs-XXXXXX", NULL);
  g_autofree char *outside = g_build_filename (root, "outside", NULL);
  g_autofree char *link = g_build_filename (root, "link", NULL);
  g_autofree char *contents = NULL;
  g_autoptr (GError) error = NULL;
  gsize length = 0;
  int fd;

  g_assert_nonnull (root);
  g_assert_true (g_file_set_contents (outside, "outside", -1, NULL));
  g_assert_cmpint (symlink (outside, link), ==, 0);

  fd = nuvashalauncher_fs_open_file_nofollow (root, "link", O_RDONLY, 0755, 0644,
                                  &error);
  g_assert_cmpint (fd, ==, -1);
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_assert_false (nuvashalauncher_fs_make_directory_tree_nofollow (root, "link/deep",
                                                        0755, &error));
  g_clear_error (&error);

  g_assert_true (nuvashalauncher_fs_get_contents_within (outside, 64, &contents, &length,
                                             &error));
  g_assert_cmpuint (length, ==, 7);
  g_assert_cmpstr (contents, ==, "outside");
  g_clear_pointer (&contents, g_free);

  g_assert_false (nuvashalauncher_fs_get_contents_within (outside, 3, &contents, &length,
                                              &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (nuvashalauncher_fs_directory_is_real (link));
  g_assert_true (nuvashalauncher_fs_directory_is_real (root));

  {
    g_autofree char *double_slash = g_strconcat (root, "//link", NULL);
    g_autofree char *parent_escape = g_strconcat (root, "/..", NULL);

    g_assert_false (nuvashalauncher_fs_directory_is_real (double_slash));
    g_assert_false (nuvashalauncher_fs_directory_is_real (parent_escape));
    g_assert_cmpint (nuvashalauncher_fs_open_directory_nofollow (double_slash, &error),
                     ==, -1);
    g_clear_error (&error);
    g_assert_cmpint (nuvashalauncher_fs_open_directory_nofollow (parent_escape, &error),
                     ==, -1);
    g_clear_error (&error);
    g_assert_cmpint (nuvashalauncher_fs_open_file_nofollow (root, "", O_RDONLY, 0755,
                                                0644, &error),
                     ==, -1);
    g_clear_error (&error);
    g_assert_cmpint (nuvashalauncher_fs_open_file_nofollow (root, "../outside", O_RDONLY,
                                                0755, 0644, &error),
                     ==, -1);
    g_clear_error (&error);
  }

  {
    int directory_fd = nuvashalauncher_fs_open_directory_nofollow (root, &error);
    g_assert_cmpint (directory_fd, >=, 0);
    g_assert_true (nuvashalauncher_fs_unlink_at (directory_fd, "link", &error));
    g_assert_cmpint (symlink (outside, link), ==, 0);
    g_assert_true (nuvashalauncher_fs_rename_at (directory_fd, "link", "renamed",
                                     &error));
    g_assert_true (nuvashalauncher_fs_unlink_at (directory_fd, "renamed", &error));
    close (directory_fd);
  }

  /* Additional tests for nuvashalauncher_fs functions */
  {
    g_autofree char *test_dir = g_build_filename (root, "nested", "dir", NULL);
    g_autofree char *file_path = g_build_filename (test_dir, "test.txt", NULL);
    g_autofree char *read_buf = NULL;
    gsize read_len = 0;

    g_assert_true (nuvashalauncher_fs_make_directory (test_dir, 0755, &error));
    g_assert_no_error (error);

    g_assert_true (nuvashalauncher_fs_replace_contents (file_path, "hello fs", 8, 0644, &error));
    g_assert_no_error (error);

    g_assert_true (nuvashalauncher_fs_get_contents_within (file_path, 1024, &read_buf, &read_len, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (read_len, ==, 8);
    g_assert_cmpstr (read_buf, ==, "hello fs");
    g_clear_pointer (&read_buf, g_free);

    /* Test nuvashalauncher_fs_unlink and rename */
    g_autofree char *renamed_path = g_build_filename (test_dir, "renamed.txt", NULL);
    g_assert_true (nuvashalauncher_fs_rename (file_path, renamed_path, &error));
    g_assert_no_error (error);
    g_assert_true (g_file_test (renamed_path, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (file_path, G_FILE_TEST_EXISTS));

    g_assert_true (nuvashalauncher_fs_unlink (renamed_path, &error));
    g_assert_no_error (error);
    g_assert_false (g_file_test (renamed_path, G_FILE_TEST_EXISTS));

    /* Test error paths */
    g_assert_false (nuvashalauncher_fs_get_contents_within (NULL, 10, &read_buf, NULL, &error));
    g_assert_nonnull (error);
    g_clear_error (&error);

    g_assert_false (nuvashalauncher_fs_get_contents_within (renamed_path, -1, &read_buf, NULL, &error));
    g_assert_nonnull (error);
    g_clear_error (&error);
  }

  nuvashalauncher_fs_remove_tree (root);
  g_print ("ok - filesystem nofollow and bounds checks\n");
  return 0;
}
