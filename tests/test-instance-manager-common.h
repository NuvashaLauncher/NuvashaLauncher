/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncherInstanceManager: persistence, atomic saves, duplication,
 * deletion and path validation. Only temporary directories are used
 * (never the real data directory) and everything runs headless: no
 * application is ever launched. */

#include "nuvashalauncher-instance-manager.h"

#include "nuvashalauncher-fs.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  GError *error;
  gpointer result;
  gboolean success;
  gboolean timed_out;
  guint watchdog;
  nuvashalauncherTaskScope *operation;
} AsyncCall;

static inline inline char *
make_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("nuvashalauncher-test-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);
  return dir;
}

/* Synchronous recursive delete, test cleanup only. Symlinks are
 * removed, not followed. */
static inline void
remove_recursive (const char *path)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInfo) info =
    g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);

  if (info == NULL)
    return;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr (GFileEnumerator) enumerator =
        g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   NULL, &error);

      if (enumerator != NULL)
        {
          GFileInfo *child = NULL;

          while ((child =
                    g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
            {
              g_autoptr (GFile) child_file =
                g_file_get_child (file, g_file_info_get_name (child));
              g_autofree char *child_path = g_file_get_path (child_file);
              g_object_unref (child);
              remove_recursive (child_path);
            }
        }
    }

  g_file_delete (file, NULL, NULL);
}

static inline void
write_file (const char *path, const char *contents)
{
  g_autofree char *parent = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);

  if (!g_file_set_contents (path, contents, -1, &error))
    g_error ("cannot write %s: %s", path, error->message);
}

static inline inline char *
read_file (const char *path)
{
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("cannot read %s: %s", path, error->message);

  return contents;
}

static inline guint
count_entries (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  guint count = 0;

  if (dir == NULL)
    return 0;

  while (g_dir_read_name (dir) != NULL)
    count++;

  g_dir_close (dir);
  return count;
}

static inline inline char *
prop_string (nuvashalauncherInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

static inline guint
manager_count (nuvashalauncherInstanceManager *manager)
{
  return g_list_model_get_n_items (
    G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (manager)));
}

/* ---------------------------------------------------------- *
 * Shared test helpers end here.
 */
