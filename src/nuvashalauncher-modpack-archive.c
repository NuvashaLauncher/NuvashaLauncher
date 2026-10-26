/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-modpack-private.h"

#include "nuvashalauncher-json.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-task-private.h"

#include <errno.h>
#include <fcntl.h>
#include <archive.h>
#include <unistd.h>
#include <archive_entry.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define nuvashalauncher_MODPACK_FORMAT_VERSION 1

/* Maximum inflated archive size. Tests may lower this limit. */
static guint64 max_content_size = nuvashalauncher_MODPACK_MAX_CONTENT_SIZE;

void
nuvashalauncher_modpack_set_content_limit (guint64 bytes)
{
  max_content_size = bytes;
}

guint64
nuvashalauncher_modpack_get_content_limit (void)
{
  return max_content_size;
}

GQuark
nuvashalauncher_modpack_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-modpack-error");
}

/* ---------------------------------------------------------- *
 * The erase side
 * ---------------------------------------------------------- */

void
nuvashalauncher_modpack_file_free (nuvashalauncherModpackFile *file)
{
  if (file == NULL)
    return;

  g_free (file->path);
  g_free (file->sha1);
  g_free (file->sha512);
  g_clear_pointer (&file->downloads, g_ptr_array_unref);
  g_free (file);
}

void
nuvashalauncher_modpack_dependency_free (nuvashalauncherModpackDependency *dependency)
{
  if (dependency == NULL)
    return;

  g_free (dependency->name);
  g_free (dependency->version);
  g_free (dependency);
}

void
nuvashalauncher_modpack_free (nuvashalauncherModpack *modpack)
{
  if (modpack == NULL)
    return;

  g_free (modpack->game);
  g_free (modpack->version_id);
  g_free (modpack->name);
  g_free (modpack->summary);
  g_free (modpack->icon);
  g_free (modpack->archive_path);
  g_free (modpack->minecraft_version);
  g_free (modpack->loader_version);
  g_clear_pointer (&modpack->files, g_ptr_array_unref);
  g_clear_pointer (&modpack->dependencies, g_ptr_array_unref);
  g_free (modpack);
}

/* ---------------------------------------------------------- *
 * From the archive
 * ---------------------------------------------------------- */

/* Validate entry metadata before reading entry data. Links are rejected:
 * symlinks can escape the instance and hardlinks can alias unrelated files. */
gboolean
nuvashalauncher_modpack_archive_entry_is_safe (const char *name, guint32 type,
                                    const char *hardlink, GError **error)
{
  if (type == AE_IFLNK || hardlink != NULL)
    {
      g_set_error_literal (error, nuvashalauncher_MODPACK_ERROR,
                           nuvashalauncher_MODPACK_ERROR_PATH,
                           "the archive holds a symlink or a hardlink");
      return FALSE;
    }

  if (type != AE_IFDIR && type != AE_IFREG)
    {
      g_set_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_ARCHIVE,
                   "\"%s\" is not a file or a directory", name != NULL ? name : "?");
      return FALSE;
    }

  return TRUE;
}

/* The payload of one entry of an archive, capped so that one entry
 * cannot carry a manifest it cannot possibly be. */
static GBytes *
archive_read_entry (struct archive *archive, guint64 limit, GError **error)
{
  g_autoptr (GByteArray) bytes = g_byte_array_new ();

  for (;;)
    {
      const void *block = NULL;
      size_t size = 0;
      la_int64_t offset = 0;
      int status = archive_read_data_block (archive, &block, &size, &offset);

      if (status == ARCHIVE_EOF)
        break;

      if (status != ARCHIVE_OK)
        {
          g_set_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_ARCHIVE,
                       "an entry of the archive cannot be read: %s",
                       archive_error_string (archive));
          return NULL;
        }

      if ((guint64) bytes->len + size > limit)
        {
          g_set_error_literal (error, nuvashalauncher_MODPACK_ERROR,
                               nuvashalauncher_MODPACK_ERROR_BOMB,
                               "an entry of the archive decodes past the "
                               "size one manifest wants");
          return NULL;
        }

      g_byte_array_append (bytes, block, (guint) size);
    }

  return g_byte_array_free_to_bytes (g_steal_pointer (&bytes));
}

#define MODRINTH_INDEX "modrinth.index.json"

/* The index of the pack from a zip, and only a safe one: every entry
 * is checked on the way in. */
nuvashalauncherModpack *
nuvashalauncher_modpack_new_from_zip (const char *zip_path, GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  g_autoptr (GBytes) index_bytes = NULL;
  guint64 paths = 0;
  guint64 size = 0;
  int status = ARCHIVE_OK;
  gboolean has_root_icon = FALSE;
  gboolean has_override_icon = FALSE;

  g_return_val_if_fail (zip_path != NULL, NULL);

  archive = archive_read_new ();

  int fd = open (zip_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

  if (fd < 0 || archive_read_support_format_zip (archive) != ARCHIVE_OK ||
      archive_read_open_fd (archive, fd, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_ARCHIVE,
                   "the modpack cannot be read: %s",
                   archive_error_string (archive));
      if (fd >= 0)
        close (fd);
      archive_read_free (archive);
      return NULL;
    }

  while ((status = archive_read_next_header (archive, &header)) == ARCHIVE_OK)
    {
      const char *name = NULL;
      g_autofree char *path = NULL;
      la_int64_t entry_size = 0;

      paths++;

      /* The bombs a manifest would deflate to the size of the disk: an
       * archive with no limit on its entries, or that announces more
       * content than a pack can have, fails before the index is even
       * asked for. */
      if (paths > nuvashalauncher_MODPACK_MAX_ENTRIES)
        {
          g_set_error_literal (error, nuvashalauncher_MODPACK_ERROR,
                               nuvashalauncher_MODPACK_ERROR_BOMB,
                               "the archive has more entries than one pack");
          goto error;
        }

      name = archive_entry_pathname (header);

      if (name == NULL ||
          !nuvashalauncher_modpack_archive_entry_is_safe (
            name, archive_entry_filetype (header),
            archive_entry_hardlink (header), error))
        goto error;

      entry_size = archive_entry_size (header);

      /* Strip a directory's trailing slash before validating its path. */
      if (archive_entry_filetype (header) == AE_IFDIR &&
          name[0] != '\0' && name[strlen (name) - 1] == '/')
        path = g_strndup (name, strlen (name) - 1);
      else
        path = g_strdup (name);

      if (!nuvashalauncher_modpack_path_is_safe (path))
        {
          g_set_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_PATH,
                       "the archive names \"%s\", which is not a path the "
                       "instance can hold",
                       name);
          goto error;
        }

      if (g_str_equal (path, "icon.png"))
        has_root_icon = TRUE;
      else if (g_str_equal (path, "overrides/icon.png"))
        has_override_icon = TRUE;

      if (entry_size > 0)
        {
          size += (guint64) entry_size;

          if (size > max_content_size)
            {
              g_set_error_literal (
                error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_BOMB,
                "the archive carries more content than one pack");
              goto error;
            }
        }

      if (g_str_equal (path, MODRINTH_INDEX) &&
          archive_entry_filetype (header) == AE_IFREG)
        {
          index_bytes = archive_read_entry (archive, 8 * 1024 * 1024, error);

          if (index_bytes == NULL)
            goto error;

          continue;
        }

      if (archive_read_data_skip (archive) != ARCHIVE_OK)
        {
          g_set_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_ARCHIVE,
                       "an entry of the archive cannot be skipped: %s",
                       archive_error_string (archive));
          goto error;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_ARCHIVE,
                   "the archive cannot be read all the way through: %s",
                   archive_error_string (archive));
      goto error;
    }

  archive_read_close (archive);
  archive_read_free (archive);

  if (index_bytes == NULL)
    {
      g_set_error_literal (error, nuvashalauncher_MODPACK_ERROR,
                           nuvashalauncher_MODPACK_ERROR_FORMAT,
                           "the archive has no modrinth.index.json at its "
                           "root");
      return NULL;
    }

  {
    gsize length = 0;
    const char *data = g_bytes_get_data (index_bytes, &length);

    /* A JSON document holds no raw NUL byte: a name that smuggles one
     * past JSON would come out of the parser as two strings. Refusing
     * the document for it stops the path from being one name where the
     * parser saw another. */
    if (memchr (data, '\0', length) != NULL)
      {
        g_set_error_literal (error, nuvashalauncher_MODPACK_ERROR,
                             nuvashalauncher_MODPACK_ERROR_PATH,
                             "the index carries a NUL byte");
        return NULL;
      }

    pack = nuvashalauncher_modpack_new_from_json (data, (gssize) length, error);

    if (pack == NULL)
      return NULL;
  }

  pack->archive_path = g_strdup (zip_path);
  /* nuvashalauncher stores its local instance icon as an ordinary override file. This
   * does not add fields to modrinth.index.json or change the .mrpack schema;
   * other launchers simply install the harmless extra override. */
  /* Both root icon.png and overrides/icon.png land at icon.png after the
   * override prefix is stripped during installation. */
  if (has_root_icon || has_override_icon)
    pack->icon = g_strdup ("icon.png");

  return g_steal_pointer (&pack);

error:

  archive_read_close (archive);
  archive_read_free (archive);

  return NULL;
}

typedef struct {
  char *path;
  nuvashalauncherTaskScope *scope;
} ModpackRead;

static void
modpack_read_free (ModpackRead *read)
{
  g_free (read->path);
  g_free (read);
}

static void
modpack_read_thread (GTask *task G_GNUC_UNUSED,
                     gpointer source_object G_GNUC_UNUSED,
                     gpointer task_data, GCancellable *cancellable)
{
  ModpackRead *read = task_data;
  g_autoptr (GError) error = NULL;
  nuvashalauncherModpack *pack = nuvashalauncher_modpack_new_from_zip (read->path, &error);

  if (g_cancellable_is_cancelled (cancellable))
    {
      nuvashalauncher_modpack_free (pack);
      nuvashalauncher_task_scope_return_error (
        read->scope,
        g_error_new_literal (nuvashalauncher_MODPACK_ERROR,
                             nuvashalauncher_MODPACK_ERROR_CANCELLED, "cancelled"));
      return;
    }

  if (pack == NULL)
    {
      nuvashalauncher_task_scope_return_error (read->scope, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_task_scope_return_pointer (read->scope, pack,
                                 (GDestroyNotify) nuvashalauncher_modpack_free);
}

nuvashalauncherTaskScope *
nuvashalauncher_modpack_new_from_zip_async (const char *zip_path,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvashalauncher_task_scope_task (scope);
  ModpackRead *read = NULL;

  g_return_val_if_fail (zip_path != NULL, NULL);

  read = g_new0 (ModpackRead, 1);

  /* Borrowed: the read is the task's own data and the scope owns that
     task. The caller keeps the operation alive until its callback. */
  read->scope = scope;
  read->path = g_strdup (zip_path);
  g_task_set_task_data (task, read, (GDestroyNotify) modpack_read_free);
  g_task_run_in_thread (task, modpack_read_thread);

  return g_steal_pointer (&scope);
}

nuvashalauncherModpack *
nuvashalauncher_modpack_new_from_zip_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
