/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "leppoisa-modpack.h"
#include "leppoisa-fs.h"
#include "leppoisa-modpack-private.h"

#include "leppoisa-json.h"
#include "leppoisa-i18n.h"
#include "leppoisa-loaders.h"
#include "leppoisa-task-private.h"

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

#define leppoisa_MODPACK_FORMAT_VERSION 1

/* The cap on what an archive may hold, leppoisa_MODPACK_MAX_CONTENT_SIZE
 * unless a caller lowered it. A test has no way to build a four gigabyte
 * archive to cross the real cap, so the bound is settable; nothing in
 * the launcher changes it. */
static guint64 max_content_size = leppoisa_MODPACK_MAX_CONTENT_SIZE;

void
leppoisa_modpack_set_content_limit (guint64 bytes)
{
  max_content_size = bytes;
}

guint64
leppoisa_modpack_get_content_limit (void)
{
  return max_content_size;
}

GQuark
leppoisa_modpack_error_quark (void)
{
  return g_quark_from_static_string ("leppoisa-modpack-error");
}

/* ---------------------------------------------------------- *
 * Reading JSON
 * ---------------------------------------------------------- */

/* The JSON of @json as a node tree, or NULL with @error set. The
 * node outlives the parser, so the caller frees it. */
static JsonNode *
parse_json (const char *json, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) parse_error = NULL;
  JsonNode *root = NULL;

  if (!json_parser_load_from_data (parser, json != NULL ? json : "",
                                   json != NULL ? length : 0, &parse_error))
    {
      g_propagate_error (error, g_steal_pointer (&parse_error));
      return NULL;
    }

  root = json_parser_get_root (parser);

  if (root == NULL)
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_FORMAT,
                           "the index holds no JSON document");
      return NULL;
    }

  return json_node_copy (root);
}

/* ---------------------------------------------------------- *
 * The erase side
 * ---------------------------------------------------------- */

void
leppoisa_modpack_file_free (LeppoisaModpackFile *file)
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
leppoisa_modpack_dependency_free (LeppoisaModpackDependency *dependency)
{
  if (dependency == NULL)
    return;

  g_free (dependency->name);
  g_free (dependency->version);
  g_free (dependency);
}

void
leppoisa_modpack_free (LeppoisaModpack *modpack)
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
 * Path safety
 * ---------------------------------------------------------- */

gboolean
leppoisa_modpack_path_is_safe (const char *path)
{
  g_auto (GStrv) parts = NULL;

  if (path == NULL || *path == '\0')
    return FALSE;

  /* Every byte a name has goes through here before it is a path on the
   * disk: a control character (NUL is one of them) or a DEL has no
   * business inside a file name. */
  for (const char *c = path; *c != '\0'; c++)
    if ((guchar) *c < 0x20 || (guchar) *c == 0x7f)
      return FALSE;

  /* A pack written for another system may use the \ as the separator:
   * it must not be a separator on this one, and a traversal written
   * with it must not turn into one. */
  if (strchr (path, '\\') != NULL)
    return FALSE;

  if (g_path_is_absolute (path))
    return FALSE;

  /* A drive name, which a path has no business dragging to a client
   * that has no drives: "C:/mods". */
  if (g_ascii_isalpha (path[0]) && path[1] == ':' &&
      (path[2] == '\0' || path[2] == '/'))
    return FALSE;

  parts = g_strsplit (path, "/", -1);

  for (guint i = 0; parts[i] != NULL; i++)
    {
      /* "" names the point to skip over: a // or a trailing / is not
       * part of a name a pack may use. "." resolves to itself but a ".."
       * is a step out of the instance, and a name may not name an
       * empty part. */
      if (parts[i][0] == '\0')
        return FALSE;

      if (g_strcmp0 (parts[i], ".") == 0 ||
          g_strcmp0 (parts[i], "..") == 0)
        return FALSE;
    }

  return TRUE;
}

/* ---------------------------------------------------------- *
 * Download URL allow list
 * ---------------------------------------------------------- */

/* The hosts the format has for the files a pack installs. The check is
 * a strict comparison on the authority: whatever carries credentials
 * or a port of its own answers as some other host, and is refused. */
static const char *allowed_hosts[] = {
  "cdn.modrinth.com",
  "github.com",
  "raw.githubusercontent.com",
  "gitlab.com",
};

gboolean
leppoisa_modpack_download_url_allowed (const char *url)
{
  const char *authority = NULL;
  const char *rest = NULL;
  gsize length = 0;
  gsize i = 0;

  if (url == NULL || !g_str_has_prefix (url, "https://"))
    return FALSE;

  authority = url + strlen ("https://");
  rest = strchr (authority, '/');

  length = rest != NULL ? (gsize) (rest - authority) : strlen (authority);

  if (length == 0)
    return FALSE;

  for (i = 0; i < G_N_ELEMENTS (allowed_hosts); i++)
    if (length == strlen (allowed_hosts[i]) &&
        g_str_has_prefix (authority, allowed_hosts[i]))
      return TRUE;

  return FALSE;
}

/* ---------------------------------------------------------- *
 * From a manifest
 * ---------------------------------------------------------- */

static LeppoisaModpackEnv
env_from_node (JsonNode *env_node, const char *name)
{
  char *value = leppoisa_json_string (env_node, name);
  LeppoisaModpackEnv env = leppoisa_MODPACK_ENV_UNSPECIFIED;

  if (value == NULL)
    return env;

  if (g_str_equal (value, "required"))
    env = leppoisa_MODPACK_ENV_REQUIRED;
  else if (g_str_equal (value, "optional"))
    env = leppoisa_MODPACK_ENV_OPTIONAL;
  else if (g_str_equal (value, "unsupported"))
    env = leppoisa_MODPACK_ENV_UNSUPPORTED;

  g_free (value);

  return env;
}

/* ---------------------------------------------------------- *
 * One entry of files[]
 * ---------------------------------------------------------- */

static LeppoisaModpackFile *
file_from_node (JsonNode *node, GError **error)
{
  g_autoptr (LeppoisaModpackFile) file = g_new0 (LeppoisaModpackFile, 1);
  g_autofree char *path = leppoisa_json_string (node, "path");
  g_autofree char *sha1 = NULL;
  g_autofree char *sha512 = NULL;
  JsonNode *hashes = NULL;
  JsonNode *downloads_node = NULL;
  JsonObject *object = NULL;

  object = leppoisa_json_object_of (node);

  if (object == NULL)
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_FORMAT,
                           "an entry of files is not an object");
      return NULL;
    }

  if (path == NULL || *path == '\0')
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_FORMAT,
                           "an entry of files has no path");
      return NULL;
    }

  if (!leppoisa_modpack_path_is_safe (path))
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_PATH,
                   "the manifest wants \"%s\", which is not a path the "
                   "instance can hold",
                   path);
      return NULL;
    }

  hashes = json_object_get_member (object, "hashes");

  if (hashes == NULL || !JSON_NODE_HOLDS_OBJECT (hashes))
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_FORMAT,
                   "\"%s\" has no hashes object", path);
      return NULL;
    }

  sha1 = leppoisa_json_string (hashes, "sha1");
  sha512 = leppoisa_json_string (hashes, "sha512");

  if (sha1 == NULL || sha512 == NULL)
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_FORMAT,
                   "\"%s\" misses the two hashes the format requires", path);
      return NULL;
    }

  downloads_node = json_object_get_member (object, "downloads");

  if (downloads_node == NULL || !JSON_NODE_HOLDS_ARRAY (downloads_node))
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_FORMAT,
                   "\"%s\" has no downloads array", path);
      return NULL;
    }

  {
    JsonArray *downloads = json_node_get_array (downloads_node);

    file->downloads = g_ptr_array_new_with_free_func (g_free);

    if (json_array_get_length (downloads) == 0)
      {
        g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_FORMAT,
                     "\"%s\" has no download URL", path);
        return NULL;
      }

    for (guint i = 0; i < json_array_get_length (downloads); i++)
      {
        g_autofree char *url =
          json_node_dup_string (json_array_get_element (downloads, i));

        if (url == NULL || *url == '\0')
          {
            g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_FORMAT,
                         "\"%s\" has an empty download URL", path);
            return NULL;
          }

        g_ptr_array_add (file->downloads, g_steal_pointer (&url));
      }
  }

  file->path = g_steal_pointer (&path);
  file->sha1 = g_steal_pointer (&sha1);
  file->sha512 = g_steal_pointer (&sha512);
  file->file_size = (goffset) leppoisa_json_int (node, "fileSize", 0);

  {
    JsonNode *env = json_object_get_member (object, "env");

    file->env_client = env_from_node (env, "client");
    file->env_server = env_from_node (env, "server");
  }

  return g_steal_pointer (&file);
}

LeppoisaModpack *
leppoisa_modpack_new_from_json (const char *json, gssize length, GError **error)
{
  g_autoptr (LeppoisaModpack) pack = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *game = NULL;
  gint64 format_version = 0;
  JsonObject *object = NULL;

  root = parse_json (json, length, error);

  if (root == NULL)
    return NULL;

  object = leppoisa_json_object_of (root);

  if (object == NULL)
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_FORMAT,
                           "the index is not a JSON object");
      return NULL;
    }

  format_version = leppoisa_json_int (root, "formatVersion", -1);

  if (format_version != leppoisa_MODPACK_FORMAT_VERSION)
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_FORMAT,
                   "the format version \"%ld\" is one this launcher does "
                   "not know",
                   (long) format_version);
      return NULL;
    }

  game = leppoisa_json_string (root, "game");

  if (g_strcmp0 (game, "minecraft") != 0)
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_FORMAT,
                   "this launcher only installs Minecraft modpacks, and the "
                   "index holds \"%s\"",
                   game != NULL ? game : "(none)");
      return NULL;
    }

  pack = g_new0 (LeppoisaModpack, 1);
  pack->format_version = (guint) format_version;
  pack->game = g_strdup (game);
  pack->version_id = leppoisa_json_string (root, "versionId");
  pack->name = leppoisa_json_string (root, "name");
  pack->summary = leppoisa_json_string (root, "summary");
  pack->files = g_ptr_array_new_with_free_func ((GDestroyNotify)
    leppoisa_modpack_file_free);
  pack->dependencies = g_ptr_array_new_with_free_func ((GDestroyNotify)
    leppoisa_modpack_dependency_free);
  pack->loader = leppoisa_LOADER_VANILLA;

  if (pack->name == NULL || *pack->name == '\0')
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_FORMAT,
                            "the index has no name");
      return NULL;
    }

  {
    JsonNode *files_node = json_object_get_member (object, "files");

    if (files_node == NULL || !JSON_NODE_HOLDS_ARRAY (files_node))
      {
        g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                             leppoisa_MODPACK_ERROR_FORMAT,
                             "the index has no files array");
        return NULL;
      }

    JsonArray *files = json_node_get_array (files_node);

    for (guint i = 0; i < json_array_get_length (files); i++)
      {
        LeppoisaModpackFile *file =
          file_from_node (json_array_get_element (files, i), error);

        if (file == NULL)
          return NULL;

        g_ptr_array_add (pack->files, file);
      }
  }

  {
    JsonNode *dependencies_node = json_object_get_member (object,
                                                          "dependencies");

    if (dependencies_node != NULL &&
        !JSON_NODE_HOLDS_OBJECT (dependencies_node))
      {
        g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                             leppoisa_MODPACK_ERROR_FORMAT,
                             "dependencies is not a JSON object");
        return NULL;
      }

    if (dependencies_node != NULL)
      {
        JsonObject *dependencies = json_node_get_object (dependencies_node);
        JsonObjectIter iter;
        const char *member_name = NULL;
        JsonNode *member_value = NULL;

        json_object_iter_init (&iter, dependencies);

        while (json_object_iter_next (&iter, &member_name, &member_value))
          {
            g_autoptr (LeppoisaModpackDependency) dep =
              g_new0 (LeppoisaModpackDependency, 1);

            dep->name = g_strdup (member_name);

            if (member_value != NULL &&
                JSON_NODE_HOLDS_VALUE (member_value) &&
                json_node_get_value_type (member_value) == G_TYPE_STRING)
              dep->version = json_node_dup_string (member_value);

            if (dep->version == NULL || *dep->version == '\0')
              {
                g_set_error (error, leppoisa_MODPACK_ERROR,
                             leppoisa_MODPACK_ERROR_FORMAT,
                             "dependency \"%s\" has no version", dep->name);
                return NULL;
              }

            g_ptr_array_add (pack->dependencies, g_steal_pointer (&dep));
          }
      }
  }

  return g_steal_pointer (&pack);
}

/* One out of the name->version pairs, or NULL. Borrowed. */
static const char *
dependency_version (LeppoisaModpack *modpack, const char *name)
{
  for (guint i = 0; i < modpack->dependencies->len; i++)
    {
      LeppoisaModpackDependency *dependency =
        g_ptr_array_index (modpack->dependencies, i);

      if (g_str_equal (dependency->name, name))
        return dependency->version;
    }

  return NULL;
}

gboolean
leppoisa_modpack_resolve (LeppoisaModpack *modpack, GError **error)
{
  static const struct {
    const char *name;
    LeppoisaLoader loader;
  } loaders[] = {
    { "fabric-loader", leppoisa_LOADER_FABRIC },
    { "quilt-loader", leppoisa_LOADER_QUILT },
    { "forge", leppoisa_LOADER_FORGE },
    { "neoforge", leppoisa_LOADER_NEOFORGE },
  };

  const char *minecraft = NULL;

  g_return_val_if_fail (modpack != NULL, FALSE);

  minecraft = dependency_version (modpack, "minecraft");

  if (minecraft == NULL || *minecraft == '\0')
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_LOADER,
                           "the pack names no Minecraft version");
      return FALSE;
    }

  modpack->minecraft_version = g_strdup (minecraft);
  modpack->loader = leppoisa_LOADER_VANILLA;
  g_clear_pointer (&modpack->loader_version, g_free);

  /* One name in what is effectively a set of loader names selects what
   * runs the pack; anything else that a future format adds is
   * ignored, not refused. */
  for (guint i = 0; i < G_N_ELEMENTS (loaders); i++)
    {
      const char *version = dependency_version (modpack, loaders[i].name);

      if (version != NULL && *version != '\0')
        {
          modpack->loader = loaders[i].loader;
          modpack->loader_version = g_strdup (version);
          break;
        }
    }

  return TRUE;
}

GPtrArray *
leppoisa_modpack_required_client_files (LeppoisaModpack *modpack)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (NULL);

  g_return_val_if_fail (modpack != NULL, files);

  for (guint i = 0; i < modpack->files->len; i++)
    {
      LeppoisaModpackFile *file = g_ptr_array_index (modpack->files, i);

      /* A file with no env object belongs to what both sides can run:
       * the client can run everything it is not told to refuse. */
      if (file->env_client == leppoisa_MODPACK_ENV_UNSUPPORTED ||
          file->env_client == leppoisa_MODPACK_ENV_OPTIONAL)
        continue;

      g_ptr_array_add (files, file);
    }

  return files;
}

GPtrArray *
leppoisa_modpack_optional_client_files (LeppoisaModpack *modpack)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (NULL);

  g_return_val_if_fail (modpack != NULL, files);

  for (guint i = 0; i < modpack->files->len; i++)
    {
      LeppoisaModpackFile *file = g_ptr_array_index (modpack->files, i);

      if (file->env_client == leppoisa_MODPACK_ENV_OPTIONAL)
        g_ptr_array_add (files, file);
    }

  return files;
}

/* ---------------------------------------------------------- *
 * From the archive
 * ---------------------------------------------------------- */

/* The entries of an archive are read through in front of any byte
 * they may carry: a link is never what a pack wants. A symlink would
 * point out of the instance, and a hardlink would share bytes with a
 * file that is no one's business here. */
gboolean
leppoisa_modpack_archive_entry_is_safe (const char *name, guint32 type,
                                    const char *hardlink, GError **error)
{
  if (type == AE_IFLNK || hardlink != NULL)
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_PATH,
                           "the archive holds a symlink or a hardlink");
      return FALSE;
    }

  if (type != AE_IFDIR && type != AE_IFREG)
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_ARCHIVE,
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
          g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_ARCHIVE,
                       "an entry of the archive cannot be read: %s",
                       archive_error_string (archive));
          return NULL;
        }

      if ((guint64) bytes->len + size > limit)
        {
          g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                               leppoisa_MODPACK_ERROR_BOMB,
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
LeppoisaModpack *
leppoisa_modpack_new_from_zip (const char *zip_path, GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  g_autoptr (LeppoisaModpack) pack = NULL;
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
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_ARCHIVE,
                   "the modpack cannot be read: %s",
                   archive_error_string (archive));
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
      if (paths > leppoisa_MODPACK_MAX_ENTRIES)
        {
          g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                               leppoisa_MODPACK_ERROR_BOMB,
                               "the archive has more entries than one pack");
          goto error;
        }

      name = archive_entry_pathname (header);

      if (name == NULL ||
          !leppoisa_modpack_archive_entry_is_safe (
            name, archive_entry_filetype (header),
            archive_entry_hardlink (header), error))
        goto error;

      entry_size = archive_entry_size (header);

      /* A directory ends in a /: what is a safe name under the root is
       * a safe name without the / under it too. */
      if (archive_entry_filetype (header) == AE_IFDIR &&
          name[0] != '\0' && name[strlen (name) - 1] == '/')
        path = g_strndup (name, strlen (name) - 1);
      else
        path = g_strdup (name);

      if (!leppoisa_modpack_path_is_safe (path))
        {
          g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_PATH,
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
                error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_BOMB,
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
          g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_ARCHIVE,
                       "an entry of the archive cannot be skipped: %s",
                       archive_error_string (archive));
          goto error;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, leppoisa_MODPACK_ERROR, leppoisa_MODPACK_ERROR_ARCHIVE,
                   "the archive cannot be read all the way through: %s",
                   archive_error_string (archive));
      goto error;
    }

  archive_read_close (archive);
  archive_read_free (archive);

  if (index_bytes == NULL)
    {
      g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                           leppoisa_MODPACK_ERROR_FORMAT,
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
        g_set_error_literal (error, leppoisa_MODPACK_ERROR,
                             leppoisa_MODPACK_ERROR_PATH,
                             "the index carries a NUL byte");
        return NULL;
      }

    pack = leppoisa_modpack_new_from_json (data, (gssize) length, error);

    if (pack == NULL)
      return NULL;
  }

  pack->archive_path = g_strdup (zip_path);
  /* leppoisa stores its local instance icon as an ordinary override file. This
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
  LeppoisaTaskScope *scope;
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
  LeppoisaModpack *pack = leppoisa_modpack_new_from_zip (read->path, &error);

  if (g_cancellable_is_cancelled (cancellable))
    {
      leppoisa_modpack_free (pack);
      leppoisa_task_scope_return_error (
        read->scope,
        g_error_new_literal (leppoisa_MODPACK_ERROR,
                             leppoisa_MODPACK_ERROR_CANCELLED, "cancelled"));
      return;
    }

  if (pack == NULL)
    {
      leppoisa_task_scope_return_error (read->scope, g_steal_pointer (&error));
      return;
    }

  leppoisa_task_scope_return_pointer (read->scope, pack,
                                 (GDestroyNotify) leppoisa_modpack_free);
}

LeppoisaTaskScope *
leppoisa_modpack_new_from_zip_async (const char *zip_path,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_autoptr (LeppoisaTaskScope) scope =
    leppoisa_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = leppoisa_task_scope_task (scope);
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

LeppoisaModpack *
leppoisa_modpack_new_from_zip_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
