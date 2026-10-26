/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <glib.h>

#include "nuvashalauncher-content-format.h"

static void
test_download_counts (void)
{
  g_autofree char *small = nuvashalauncher_content_format_downloads (42);
  g_autofree char *thousands = nuvashalauncher_content_format_downloads (1250);
  g_autofree char *millions = nuvashalauncher_content_format_downloads (2500000);

  g_assert_cmpstr (small, ==, "42 downloads");
  g_assert_cmpstr (thousands, ==, "1.2k downloads");
  g_assert_cmpstr (millions, ==, "2.5M downloads");
  g_print ("ok - download counts use compact localized formats\n");
}

static void
test_author_lines (void)
{
  g_autofree char *licensed =
    nuvashalauncher_content_format_author ("MIT", "Example Author");
  g_autofree char *author_only =
    nuvashalauncher_content_format_author (NULL, "Example Author");
  g_autofree char *empty_author = nuvashalauncher_content_format_author ("MIT", NULL);

  g_assert_cmpstr (licensed, ==, "MIT  ·  by Example Author");
  g_assert_cmpstr (author_only, ==, "by Example Author");
  g_assert_cmpstr (empty_author, ==, "MIT  ·  by ");
  g_print ("ok - author and license lines handle optional metadata\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Content formatting\n");
  test_download_counts ();
  test_author_lines ();
  return 0;
}
