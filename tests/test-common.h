/* Shared helpers for NuvashaLauncher unit and integration tests. */
#pragma once

#include "nuvashalauncher-fs.h"

#include <glib.h>

static inline char *
test_make_tmp_dir (const char *prefix)
{
  g_autofree char *tmpl = g_strdup_printf ("%s-XXXXXX", prefix);
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp (tmpl, &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);
  return dir;
}

static inline void
test_remove_tree (const char *path)
{
  if (path != NULL)
    nuvashalauncher_fs_remove_tree (path);
}

static inline char *
test_read_file (const char *path)
{
  char *contents = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
  g_assert_no_error (error);
  return contents;
}

static inline void
test_drain_context (void)
{
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);
}
