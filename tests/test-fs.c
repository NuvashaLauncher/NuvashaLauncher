#define _GNU_SOURCE

/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-fs.h"

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

int
main (void)
{
  g_autofree char *root = g_dir_make_tmp ("aml-fs-XXXXXX", NULL);
  g_autofree char *outside = g_build_filename (root, "outside", NULL);
  g_autofree char *link = g_build_filename (root, "link", NULL);
  g_autofree char *contents = NULL;
  g_autoptr (GError) error = NULL;
  gsize length = 0;
  int fd;

  g_assert_nonnull (root);
  g_assert_true (g_file_set_contents (outside, "outside", -1, NULL));
  g_assert_cmpint (symlink (outside, link), ==, 0);

  fd = aml_fs_open_file_nofollow (root, "link", O_RDONLY, 0755, 0644,
                                  &error);
  g_assert_cmpint (fd, ==, -1);
  g_assert_nonnull (error);
  g_clear_error (&error);

  g_assert_false (aml_fs_make_directory_tree_nofollow (root, "link/deep",
                                                        0755, &error));
  g_clear_error (&error);

  g_assert_true (aml_fs_get_contents_within (outside, 64, &contents, &length,
                                             &error));
  g_assert_cmpuint (length, ==, 7);
  g_assert_cmpstr (contents, ==, "outside");
  g_clear_pointer (&contents, g_free);

  g_assert_false (aml_fs_get_contents_within (outside, 3, &contents, &length,
                                              &error));
  g_assert_nonnull (error);
  g_assert_false (aml_fs_directory_is_real (link));
  g_assert_true (aml_fs_directory_is_real (root));

  {
    int directory_fd = aml_fs_open_directory_nofollow (root, &error);
    g_assert_cmpint (directory_fd, >=, 0);
    g_assert_true (aml_fs_unlink_at (directory_fd, "link", &error));
    g_assert_cmpint (symlink (outside, link), ==, 0);
    g_assert_true (aml_fs_rename_at (directory_fd, "link", "renamed",
                                     &error));
    g_assert_true (aml_fs_unlink_at (directory_fd, "renamed", &error));
    close (directory_fd);
  }

  g_print ("ok - filesystem nofollow and bounds checks\n");
  return 0;
}
