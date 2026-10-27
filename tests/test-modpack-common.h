/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

/* Tests for nuvashalauncher-modpack: what the index of a pack says, what its
 * dependencies resolve to, what the two hashes of a file prove, and
 * what an install leaves behind when it cannot go through.
 *
 * Everything here runs on files in a temporary directory: no network,
 * no display, nothing of the real data directory. The .mrpack
 * fixtures are written by the test itself, so no pack is ever checked
 * in. The install that succeeds never downloads anything: a pack with
 * no files and no loader installs from its overrides alone, which is
 * the whole pipeline minus the network. */

#include "nuvashalauncher-download-modpack-dialog.h"
#include "nuvashalauncher-content-index.h"
#include "nuvashalauncher-modpack.h"

#include <gtk/gtk.h>

#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

static inline inline char *
make_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("nuvashalauncher-modpack-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);
  return dir;
}

/* Synchronous recursive delete, test cleanup only. Symlinks are
 * removed, never followed. */
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

static inline inline char *
read_file (const char *path)
{
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("cannot read %s: %s", path, error->message);

  return contents;
}

/* An index of a pack with the members the format requires. @files is
 * what goes inside the files array, brackets and commas included: an
 * empty pack passes "" and one file passes "{...}". */
static inline inline char *
pack_index (const char *name, const char *files, const char *deps)
{
  return g_strdup_printf (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1.0.0\","
    "\"name\":\"%s\",\"summary\":\"a pack the tests made\","
    "\"files\":[%s],\"dependencies\":{%s}}",
    name, files, deps);
}

/* One entry of a .mrpack the test writes. */
typedef struct
{
  const char *name;
  const char *data;
  gsize size;
  guint type;       /* AE_IFREG, AE_IFDIR, AE_IFLNK */
  const char *link; /* symlink target when type is AE_IFLNK */
} ZipEntry;

/* A regular file entry out of a literal. */
#define ZIP_FILE(name, data) \
  { name, data, sizeof (data) - 1, AE_IFREG, NULL }

/* Writes a .mrpack out of the entries, in the order given. A symlink
 * entry carries its target and nothing else: the writer puts the
 * target in the entry itself, and writing data alongside it would
 * make the archive inconsistent. */
static inline void
zip_write (const char *path, const ZipEntry *entries, gsize count)
{
  struct archive *archive = archive_write_new ();

  g_assert_nonnull (archive);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==,
                   ARCHIVE_OK);

  for (gsize i = 0; i < count; i++)
    {
      struct archive_entry *entry = archive_entry_new ();

      g_assert_nonnull (entry);
      archive_entry_set_pathname (entry, entries[i].name);
      archive_entry_set_filetype (entry, entries[i].type);
      archive_entry_set_perm (entry,
                              entries[i].type == AE_IFDIR ? 0755 : 0644);

      if (entries[i].type == AE_IFLNK)
        {
          archive_entry_set_symlink (entry, entries[i].link);
          archive_entry_set_size (entry,
                                  (la_int64_t) strlen (entries[i].link));
        }
      else if (entries[i].type == AE_IFREG)
        {
          archive_entry_set_size (entry, (la_int64_t) entries[i].size);
        }

      g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);

      if (entries[i].type == AE_IFREG && entries[i].size > 0)
        g_assert_cmpint (
          (int) archive_write_data (archive, entries[i].data,
                                     entries[i].size),
          ==, (int) entries[i].size);

      archive_entry_free (entry);
    }

  g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
  archive_write_free (archive);
}

/* Whether a directory holds a temporary directory of an install. */
static inline gboolean
has_temp_dir (const char *directory)
{
  GDir *dir = g_dir_open (directory, 0, NULL);
  const char *name = NULL;
  gboolean found = FALSE;

  if (dir == NULL)
    return FALSE;

  while ((name = g_dir_read_name (dir)) != NULL)
    if (g_str_has_prefix (name, ".nuvashalauncher-modpack-"))
      {
        found = TRUE;
        break;
      }

  g_dir_close (dir);
  return found;
}

typedef struct
{
  GMainLoop *loop;
  GError *error;
  gboolean value;
  nuvashalauncherInstance *instance;
  gboolean timed_out;
  guint watchdog;
  nuvashalauncherTaskScope *operation;
} AsyncWait;

static inline gboolean
async_timeout (gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->timed_out = TRUE;
  wait->watchdog = 0;
  g_main_loop_quit (wait->loop);
  return G_SOURCE_REMOVE;
}

static inline void
async_wait_run (AsyncWait *wait)
{
  wait->loop = g_main_loop_new (NULL, FALSE);
  wait->watchdog = g_timeout_add_seconds (30, async_timeout, wait);
  g_main_loop_run (wait->loop);

  if (wait->watchdog != 0)
    g_source_remove (wait->watchdog);

  g_main_loop_unref (wait->loop);
  wait->loop = NULL;

  g_assert_false (wait->timed_out);
}

static inline void
verify_file_done (GObject *source G_GNUC_UNUSED,
                  GAsyncResult *result,
                  gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->value = nuvashalauncher_modpack_verify_file_finish (result, &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

/* Both hashes of the manifest against the bytes on disk, in place. */
static inline gboolean
verify_now (const nuvashalauncherModpackFile *file, const char *path, GError **error)
{
  AsyncWait wait = { 0 };

  wait.operation = nuvashalauncher_modpack_verify_file_async (file, path, verify_file_done,
                                                 &wait);
  async_wait_run (&wait);

  *error = wait.error;
  return wait.value;
}

static inline void
install_done (GObject *source G_GNUC_UNUSED,
              GAsyncResult *result,
              gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->instance = nuvashalauncher_modpack_install_finish (result, &wait->error);
  if (wait->operation != NULL)
    {
      nuvashalauncher_task_scope_unref (wait->operation);
      wait->operation = NULL;
    }
  g_main_loop_quit (wait->loop);
}

static inline void
export_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->value = nuvashalauncher_modpack_export_finish (result, &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

static inline void
index_update_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->value = nuvashalauncher_content_index_modify_finish (result, &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

/* The whole install, in place. */
static inline nuvashalauncherInstance *
install_now (nuvashalauncherModpack *pack, nuvashalauncherInstanceManager *manager, const char *name,
             const char *directory, GError **error)
{
  AsyncWait wait = { 0 };

   wait.operation = nuvashalauncher_modpack_install_async (
     pack, manager, name, directory, NULL, NULL, install_done, &wait);
  async_wait_run (&wait);

  *error = wait.error;
  return wait.instance;
}

static inline gboolean
export_now (nuvashalauncherInstance *instance, const char *path, GError **error)
{
  AsyncWait wait = { 0 };

  wait.operation = nuvashalauncher_modpack_export_async (instance, path, NULL, NULL, NULL,
                                             FALSE, NULL, NULL, export_done,
                                             &wait);
  async_wait_run (&wait);

  *error = wait.error;
  return wait.value;
}

static inline gboolean
index_update_now (const char *directory, nuvashalauncherContentType type,
                  nuvashalauncherContentIndexEntry *entry, GError **error)
{
  AsyncWait wait = { 0 };

  wait.operation = nuvashalauncher_content_index_update_async (
    directory, type, entry, index_update_done, &wait);
  async_wait_run (&wait);

  *error = wait.error;
  return wait.value;
}

static inline guint
manager_count (nuvashalauncherInstanceManager *manager)
{
  return g_list_model_get_n_items (
    G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (manager)));
}

/* ---------------------------------------------------------- *
 * What the index of a pack says
 * ---------------------------------------------------------- */

static inline void
assert_loader (const char *deps_json, nuvashalauncherLoader loader,
               const char *minecraft, const char *loader_version)
{
  g_autofree char *json = pack_index ("Resolve Pack", "", deps_json);
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;

  pack = nuvashalauncher_modpack_new_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvashalauncher_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (pack->minecraft_version, ==, minecraft);
  g_assert_cmpint (pack->loader, ==, loader);
  g_assert_cmpstr (pack->loader_version, ==, loader_version);
}















/* ---------------------------------------------------------- *
 * What the format refuses
 * ---------------------------------------------------------- */

static inline void
expect_refused (const char *json, guint code, const char *what)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;

  pack = nuvashalauncher_modpack_new_from_json (json, -1, &error);
  g_assert_null (pack);
  g_assert_error (error, nuvashalauncher_MODPACK_ERROR, code);
  g_print ("ok - %s\n", what);
}







/* ---------------------------------------------------------- *
 * What the two hashes of a file prove
 * ---------------------------------------------------------- */



/* ---------------------------------------------------------- *
 * What an install leaves behind
 * ---------------------------------------------------------- */

static inline nuvashalauncherInstanceManager *
manager_new (const char *data_dir)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = nuvashalauncher_instance_manager_new (data_dir);

  g_assert_nonnull (manager);
  g_assert_true (nuvashalauncher_instance_manager_load (manager, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (manager), ==, 0);

  return g_steal_pointer (&manager);
}













/* ---------------------------------------------------------- *
 * The dialog's own flow
 *
 * Reading a pack and installing it through the dialog is what a user does,
 * and it is a different path from the install this file otherwise tests:
 * the dialog hands its own references around and releases the operations
 * it started. Local only, so it runs without a network. The widgets need
 * a display; without one the flow is skipped rather than faked. */



#if defined(__SANITIZE_ADDRESS__)

#endif

/* ---------------------------------------------------------- */
