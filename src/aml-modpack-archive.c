/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "aml-modpack.h"
#include "aml-modpack-private.h"

#include "aml-json.h"
#include "aml-i18n.h"
#include "aml-loaders.h"
#include "aml-task-private.h"

#include <errno.h>
#include <fcntl.h>
#include <archive.h>
#include <archive_entry.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AML_MODPACK_FORMAT_VERSION 1

/* The cap on what an archive may hold, AML_MODPACK_MAX_CONTENT_SIZE
 * unless a caller lowered it. A test has no way to build a four gigabyte
 * archive to cross the real cap, so the bound is settable; nothing in
 * the launcher changes it. */
static guint64 max_content_size = AML_MODPACK_MAX_CONTENT_SIZE;

void
aml_modpack_set_content_limit (guint64 bytes)
{
  max_content_size = bytes;
}

guint64
aml_modpack_get_content_limit (void)
{
  return max_content_size;
}

GQuark
aml_modpack_error_quark (void)
{
  return g_quark_from_static_string ("aml-modpack-error");
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
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
                           "the index holds no JSON document");
      return NULL;
    }

  return json_node_copy (root);
}

/* ---------------------------------------------------------- *
 * The erase side
 * ---------------------------------------------------------- */

void
aml_modpack_file_free (AmlModpackFile *file)
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
aml_modpack_dependency_free (AmlModpackDependency *dependency)
{
  if (dependency == NULL)
    return;

  g_free (dependency->name);
  g_free (dependency->version);
  g_free (dependency);
}

void
aml_modpack_free (AmlModpack *modpack)
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
aml_modpack_path_is_safe (const char *path)
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
aml_modpack_download_url_allowed (const char *url)
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

static AmlModpackEnv
env_from_node (JsonNode *env_node, const char *name)
{
  char *value = aml_json_string (env_node, name);
  AmlModpackEnv env = AML_MODPACK_ENV_UNSPECIFIED;

  if (value == NULL)
    return env;

  if (g_str_equal (value, "required"))
    env = AML_MODPACK_ENV_REQUIRED;
  else if (g_str_equal (value, "optional"))
    env = AML_MODPACK_ENV_OPTIONAL;
  else if (g_str_equal (value, "unsupported"))
    env = AML_MODPACK_ENV_UNSUPPORTED;

  g_free (value);

  return env;
}

/* ---------------------------------------------------------- *
 * One entry of files[]
 * ---------------------------------------------------------- */

static AmlModpackFile *
file_from_node (JsonNode *node, GError **error)
{
  g_autoptr (AmlModpackFile) file = g_new0 (AmlModpackFile, 1);
  g_autofree char *path = aml_json_string (node, "path");
  g_autofree char *sha1 = NULL;
  g_autofree char *sha512 = NULL;
  JsonNode *hashes = NULL;
  JsonNode *downloads_node = NULL;
  JsonObject *object = NULL;

  object = aml_json_object_of (node);

  if (object == NULL)
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
                           "an entry of files is not an object");
      return NULL;
    }

  if (path == NULL || *path == '\0')
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
                           "an entry of files has no path");
      return NULL;
    }

  if (!aml_modpack_path_is_safe (path))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
                   "the manifest wants \"%s\", which is not a path the "
                   "instance can hold",
                   path);
      return NULL;
    }

  hashes = json_object_get_member (object, "hashes");

  if (hashes == NULL || !JSON_NODE_HOLDS_OBJECT (hashes))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "\"%s\" has no hashes object", path);
      return NULL;
    }

  sha1 = aml_json_string (hashes, "sha1");
  sha512 = aml_json_string (hashes, "sha512");

  if (sha1 == NULL || sha512 == NULL)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "\"%s\" misses the two hashes the format requires", path);
      return NULL;
    }

  downloads_node = json_object_get_member (object, "downloads");

  if (downloads_node == NULL || !JSON_NODE_HOLDS_ARRAY (downloads_node))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "\"%s\" has no downloads array", path);
      return NULL;
    }

  {
    JsonArray *downloads = json_node_get_array (downloads_node);

    file->downloads = g_ptr_array_new_with_free_func (g_free);

    if (json_array_get_length (downloads) == 0)
      {
        g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                     "\"%s\" has no download URL", path);
        return NULL;
      }

    for (guint i = 0; i < json_array_get_length (downloads); i++)
      {
        g_autofree char *url =
          json_node_dup_string (json_array_get_element (downloads, i));

        if (url == NULL || *url == '\0')
          {
            g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                         "\"%s\" has an empty download URL", path);
            return NULL;
          }

        g_ptr_array_add (file->downloads, g_steal_pointer (&url));
      }
  }

  file->path = g_steal_pointer (&path);
  file->sha1 = g_steal_pointer (&sha1);
  file->sha512 = g_steal_pointer (&sha512);
  file->file_size = (goffset) aml_json_int (node, "fileSize", 0);

  {
    JsonNode *env = json_object_get_member (object, "env");

    file->env_client = env_from_node (env, "client");
    file->env_server = env_from_node (env, "server");
  }

  return g_steal_pointer (&file);
}

AmlModpack *
aml_modpack_new_from_json (const char *json, gssize length, GError **error)
{
  g_autoptr (AmlModpack) pack = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *game = NULL;
  gint64 format_version = 0;
  JsonObject *object = NULL;

  root = parse_json (json, length, error);

  if (root == NULL)
    return NULL;

  object = aml_json_object_of (root);

  if (object == NULL)
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
                           "the index is not a JSON object");
      return NULL;
    }

  format_version = aml_json_int (root, "formatVersion", -1);

  if (format_version != AML_MODPACK_FORMAT_VERSION)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "the format version \"%ld\" is one this launcher does "
                   "not know",
                   (long) format_version);
      return NULL;
    }

  game = aml_json_string (root, "game");

  if (g_strcmp0 (game, "minecraft") != 0)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "this launcher only installs Minecraft modpacks, and the "
                   "index holds \"%s\"",
                   game != NULL ? game : "(none)");
      return NULL;
    }

  pack = g_new0 (AmlModpack, 1);
  pack->format_version = (guint) format_version;
  pack->game = g_strdup (game);
  pack->version_id = aml_json_string (root, "versionId");
  pack->name = aml_json_string (root, "name");
  pack->summary = aml_json_string (root, "summary");
  pack->files = g_ptr_array_new_with_free_func ((GDestroyNotify)
    aml_modpack_file_free);
  pack->dependencies = g_ptr_array_new_with_free_func ((GDestroyNotify)
    aml_modpack_dependency_free);
  pack->loader = AML_LOADER_VANILLA;

  if (pack->name == NULL || *pack->name == '\0')
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
                            "the index has no name");
      return NULL;
    }

  {
    JsonNode *files_node = json_object_get_member (object, "files");

    if (files_node == NULL || !JSON_NODE_HOLDS_ARRAY (files_node))
      {
        g_set_error_literal (error, AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_FORMAT,
                             "the index has no files array");
        return NULL;
      }

    JsonArray *files = json_node_get_array (files_node);

    for (guint i = 0; i < json_array_get_length (files); i++)
      {
        AmlModpackFile *file =
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
        g_set_error_literal (error, AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_FORMAT,
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
            g_autoptr (AmlModpackDependency) dep =
              g_new0 (AmlModpackDependency, 1);

            dep->name = g_strdup (member_name);

            if (member_value != NULL &&
                JSON_NODE_HOLDS_VALUE (member_value) &&
                json_node_get_value_type (member_value) == G_TYPE_STRING)
              dep->version = json_node_dup_string (member_value);

            if (dep->version == NULL || *dep->version == '\0')
              {
                g_set_error (error, AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_FORMAT,
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
dependency_version (AmlModpack *modpack, const char *name)
{
  for (guint i = 0; i < modpack->dependencies->len; i++)
    {
      AmlModpackDependency *dependency =
        g_ptr_array_index (modpack->dependencies, i);

      if (g_str_equal (dependency->name, name))
        return dependency->version;
    }

  return NULL;
}

gboolean
aml_modpack_resolve (AmlModpack *modpack, GError **error)
{
  static const struct {
    const char *name;
    AmlLoader loader;
  } loaders[] = {
    { "fabric-loader", AML_LOADER_FABRIC },
    { "quilt-loader", AML_LOADER_QUILT },
    { "forge", AML_LOADER_FORGE },
    { "neoforge", AML_LOADER_NEOFORGE },
  };

  const char *minecraft = NULL;

  g_return_val_if_fail (modpack != NULL, FALSE);

  minecraft = dependency_version (modpack, "minecraft");

  if (minecraft == NULL || *minecraft == '\0')
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_LOADER,
                           "the pack names no Minecraft version");
      return FALSE;
    }

  modpack->minecraft_version = g_strdup (minecraft);
  modpack->loader = AML_LOADER_VANILLA;
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
aml_modpack_required_client_files (AmlModpack *modpack)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (NULL);

  g_return_val_if_fail (modpack != NULL, files);

  for (guint i = 0; i < modpack->files->len; i++)
    {
      AmlModpackFile *file = g_ptr_array_index (modpack->files, i);

      /* A file with no env object belongs to what both sides can run:
       * the client can run everything it is not told to refuse. */
      if (file->env_client == AML_MODPACK_ENV_UNSUPPORTED ||
          file->env_client == AML_MODPACK_ENV_OPTIONAL)
        continue;

      g_ptr_array_add (files, file);
    }

  return files;
}

GPtrArray *
aml_modpack_optional_client_files (AmlModpack *modpack)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (NULL);

  g_return_val_if_fail (modpack != NULL, files);

  for (guint i = 0; i < modpack->files->len; i++)
    {
      AmlModpackFile *file = g_ptr_array_index (modpack->files, i);

      if (file->env_client == AML_MODPACK_ENV_OPTIONAL)
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
static gboolean
entry_is_safe (const char *name, guint32 type, const char *hardlink, GError **error)
{
  if (type == AE_IFLNK || hardlink != NULL)
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_PATH,
                           "the archive holds a symlink or a hardlink");
      return FALSE;
    }

  if (type != AE_IFDIR && type != AE_IFREG)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
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
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                       "an entry of the archive cannot be read: %s",
                       archive_error_string (archive));
          return NULL;
        }

      if ((guint64) bytes->len + size > limit)
        {
          g_set_error_literal (error, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_BOMB,
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
AmlModpack *
aml_modpack_new_from_zip (const char *zip_path, GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  g_autoptr (AmlModpack) pack = NULL;
  g_autoptr (GBytes) index_bytes = NULL;
  guint64 paths = 0;
  guint64 size = 0;
  int status = ARCHIVE_OK;
  gboolean has_root_icon = FALSE;
  gboolean has_override_icon = FALSE;

  g_return_val_if_fail (zip_path != NULL, NULL);

  archive = archive_read_new ();

  if (archive_read_support_format_zip (archive) != ARCHIVE_OK ||
      archive_read_open_filename (archive, zip_path, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
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
      if (paths > AML_MODPACK_MAX_ENTRIES)
        {
          g_set_error_literal (error, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_BOMB,
                               "the archive has more entries than one pack");
          goto error;
        }

      name = archive_entry_pathname (header);

      if (name == NULL ||
          !entry_is_safe (name, archive_entry_filetype (header),
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

      if (!aml_modpack_path_is_safe (path))
        {
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
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
                error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_BOMB,
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
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                       "an entry of the archive cannot be skipped: %s",
                       archive_error_string (archive));
          goto error;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "the archive cannot be read all the way through: %s",
                   archive_error_string (archive));
      goto error;
    }

  archive_read_close (archive);
  archive_read_free (archive);

  if (index_bytes == NULL)
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
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
        g_set_error_literal (error, AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_PATH,
                             "the index carries a NUL byte");
        return NULL;
      }

    pack = aml_modpack_new_from_json (data, (gssize) length, error);

    if (pack == NULL)
      return NULL;
  }

  pack->archive_path = g_strdup (zip_path);
  /* AML stores its local instance icon as an ordinary override file. This
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
  AmlTaskScope *scope;
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
  AmlModpack *pack = aml_modpack_new_from_zip (read->path, &error);

  if (g_cancellable_is_cancelled (cancellable))
    {
      aml_modpack_free (pack);
      aml_task_scope_return_error (
        read->scope,
        g_error_new_literal (AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_CANCELLED, "cancelled"));
      return;
    }

  if (pack == NULL)
    {
      aml_task_scope_return_error (read->scope, g_steal_pointer (&error));
      return;
    }

  aml_task_scope_return_pointer (read->scope, pack,
                                 (GDestroyNotify) aml_modpack_free);
}

AmlTaskScope *
aml_modpack_new_from_zip_async (const char *zip_path,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
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

AmlModpack *
aml_modpack_new_from_zip_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Checking what arrived
 * ---------------------------------------------------------- */

typedef struct
{
  char *path;
  char *sha1;
  char *sha512;
  guint stage; /* 0: checking sha1, 1: checking sha512 */
  GTask *task; /* owned */
  AmlTaskScope *scope;
} VerifyRun;

static void
verify_run_free (VerifyRun *run)
{
  if (run == NULL)
    return;

  g_free (run->path);
  g_free (run->sha1);
  g_free (run->sha512);
  g_clear_object (&run->task);
  g_free (run);
}

static void
verify_hash_ready (GObject *source G_GNUC_UNUSED,
                   GAsyncResult *result,
                   gpointer user_data)
{
  VerifyRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = NULL;
  const char *expect = NULL;

  digest = aml_download_file_hash_finish (result, &error);

  if (digest == NULL)
    {
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      verify_run_free (run);
      return;
    }

  expect = run->stage == 0 ? run->sha1 : run->sha512;

  if (g_ascii_strcasecmp (expect, digest) != 0)
    {
      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
                   "\"%s\" arrived with a different %s than the index says",
                   run->path, run->stage == 0 ? "sha1" : "sha512");
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      verify_run_free (run);
      return;
    }

  run->stage++;

  if (run->stage > 1)
    {
      aml_task_scope_return_boolean (run->scope, TRUE);
      verify_run_free (run);
      return;
    }

  {
    g_autoptr (GFile) file = g_file_new_for_path (run->path);

    aml_task_scope_release_when_done (
      aml_download_file_hash_async (file, G_CHECKSUM_SHA512,
                                    verify_hash_ready, run),
      aml_task_scope_get_cancellable (run->scope));
  }
}

/* Both hashes of the manifest, out of the bytes on the disk. */
AmlTaskScope *
aml_modpack_verify_file_async (const AmlModpackFile *file,
                               const char *path,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  g_autoptr (GFile) job_file = g_file_new_for_path (path);
  VerifyRun *run = NULL;

  g_return_val_if_fail (file != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  if (file->sha1 == NULL || file->sha512 == NULL)
    {
      aml_task_scope_return_error_deferred (
        scope,
        g_error_new (AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
                     "\"%s\" does not carry the two hashes the format "
                     "requires", file->path != NULL ? file->path : "file"));
      return g_steal_pointer (&scope);
    }

  run = g_new0 (VerifyRun, 1);
  run->path = g_strdup (path);
  run->sha1 = g_strdup (file->sha1);
  run->sha512 = g_strdup (file->sha512);
  run->stage = 0;
  run->task = g_object_ref (task);

  /* Borrowed: the operation the caller owns until the answer. */
  run->scope = scope;

  aml_task_scope_release_when_done (
    aml_download_file_hash_async (job_file, G_CHECKSUM_SHA1, verify_hash_ready,
                                  run),
    aml_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

gboolean
aml_modpack_verify_file_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Extraction of the overrides
 * ---------------------------------------------------------- */

static gboolean
extract_mkdir (const char *directory, const char *relative, GError **error)
{
  g_auto (GStrv) parts = g_strsplit (relative, "/", -1);
  int current = -1;

  current = open (directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (current < 0)
    goto failed;

  for (guint i = 0; parts[i] != NULL; i++)
    {
      int next = -1;

      if (mkdirat (current, parts[i], 0755) != 0 && errno != EEXIST)
        goto failed;

      next = openat (current, parts[i],
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        goto failed;

      close (current);
      current = next;
    }

  close (current);
  return TRUE;

failed:
  if (current >= 0)
    close (current);
  g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
               "cannot securely make \"%s\": %s", relative,
               g_strerror (errno));
  return FALSE;
}

/* Open a new regular file below @directory without following any symlink in
 * the path. The archive has already validated the spelling of @relative;
 * this second layer protects the filesystem operation itself from a path
 * component being replaced while extraction is in progress. */
static int
extract_open_file (const char *directory, const char *relative, GError **error)
{
  g_auto (GStrv) parts = g_strsplit (relative, "/", -1);
  int current = -1;
  guint last = 0;

  while (parts[last + 1] != NULL)
    last++;

  current = open (directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (current < 0)
    goto failed;

  for (guint i = 0; i < last; i++)
    {
      int next = -1;

      if (mkdirat (current, parts[i], 0755) != 0 && errno != EEXIST)
        goto failed;

      next = openat (current, parts[i],
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        goto failed;

      close (current);
      current = next;
    }

  {
    int fd = openat (current, parts[last],
                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                     0644);
    int saved_errno = errno;
    close (current);
    current = -1;
    if (fd >= 0)
      return fd;
    errno = saved_errno;
  }

failed:
  if (current >= 0)
    close (current);
  g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
               "cannot securely open \"%s\": %s", relative,
               g_strerror (errno));
  return -1;
}

static gboolean
extract_entry (struct archive *archive,
               struct archive_entry *header,
               const char *directory,
               const char *prefix,
               guint64 *used,
               GError **error)
{
  const char *name = archive_entry_pathname (header);
  g_autofree char *relative = NULL;
  FILE *out = NULL;

  if (name == NULL)
    {
      g_set_error_literal (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                           "an override has no pathname");
      return FALSE;
    }

  if (g_str_equal (prefix, "icon.png"))
    {
      if (!g_str_equal (name, prefix))
        return TRUE;
      relative = g_strdup (name);
    }
  else
    {
      if (!g_str_has_prefix (name, prefix))
        return TRUE;
      relative = g_strdup (name + strlen (prefix));
    }

  /* The prefix itself is no file. */
  if (relative[0] == '\0')
    return TRUE;

  /* AML's content indexes are launcher metadata, never game files. */
  if (g_str_has_prefix (relative, ".index/") ||
      strstr (relative, "/.index/") != NULL)
    return TRUE;

  /* A directory ends in a /: no name of its own to check against the
   * safety rules, only its contents. */
  {
    gsize length = strlen (relative);

    if (length > 0 && relative[length - 1] == '/')
      relative[length - 1] = '\0';
  }

  if (!aml_modpack_path_is_safe (relative))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
                   "the overrides hold \"%s\", which is not a path the "
                   "instance can hold",
                   name);
      return FALSE;
    }

  if (archive_entry_filetype (header) == AE_IFDIR)
    {
      return extract_mkdir (directory, relative, error);
    }

  if (archive_entry_filetype (header) != AE_IFREG)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "\"%s\" is not a file", name);
      return FALSE;
    }

  {
    int fd = extract_open_file (directory, relative, error);
    if (fd >= 0)
      out = fdopen (fd, "wb");
    if (out == NULL && fd >= 0)
      close (fd);
  }

  if (out == NULL)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "\"%s\" cannot be written: %s", name, g_strerror (errno));
      return FALSE;
    }

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
          fclose (out);
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                       "\"%s\" cannot be read out of the archive: %s", name,
                       archive_error_string (archive));
          return FALSE;
        }

      if (size != 0)
        {
          /* The bomb protection runs on the bytes that did inflate, not
           * on what the entry announced. */
          if (*used > max_content_size ||
              (guint64) size > max_content_size - *used)
            {
              fclose (out);
              g_set_error_literal (error, AML_MODPACK_ERROR,
                                   AML_MODPACK_ERROR_BOMB,
                                   "the overrides decode past the size one "
                                   "pack carries");
              return FALSE;
            }

          if (fwrite (block, 1, size, out) != size)
            {
              fclose (out);
              g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                           "\"%s\" cannot be written: %s", name,
                           g_strerror (errno));
              return FALSE;
            }

          *used += size;
        }
    }

  if (fclose (out) != 0)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "\"%s\" cannot be closed: %s", name, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

/* One archive read for one of the two folder layers of a pack. */
static gboolean
extract_layers (const char *archive_path, const char *directory,
                guint64 *used, GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  int status = ARCHIVE_OK;

  archive = archive_read_new ();

  if (archive_read_support_format_zip (archive) != ARCHIVE_OK ||
      archive_read_open_filename (archive, archive_path, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "the pack cannot be opened: %s",
                   archive_error_string (archive));
      archive_read_free (archive);
      return FALSE;
    }

  while ((status = archive_read_next_header (archive, &header)) == ARCHIVE_OK)
    {
      if (!entry_is_safe (archive_entry_pathname (header),
                          archive_entry_filetype (header),
                          archive_entry_hardlink (header), error))
        {
          archive_read_close (archive);
          archive_read_free (archive);
          return FALSE;
        }

      const char *name = archive_entry_pathname (header);
      const char *prefix = NULL;

      if (name != NULL && g_str_equal (name, "icon.png"))
        prefix = "icon.png";
      else if (name != NULL && g_str_has_prefix (name, "client-overrides/"))
        prefix = "client-overrides/";
      else if (name != NULL && g_str_has_prefix (name, "overrides/"))
        prefix = "overrides/";

      if (prefix != NULL &&
          !extract_entry (archive, header, directory, prefix, used, error))
        {
          archive_read_close (archive);
          archive_read_free (archive);
          return FALSE;
        }

      if (archive_read_data_skip (archive) != ARCHIVE_OK)
        {
          archive_read_close (archive);
          archive_read_free (archive);
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                       "the archive cannot be read all the way through");
          return FALSE;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "the overrides cannot be read: %s",
                   archive_error_string (archive));
      archive_read_close (archive);
      archive_read_free (archive);
      return FALSE;
    }

  archive_read_close (archive);
  archive_read_free (archive);

  return TRUE;
}

/* client-overrides wins over overrides, so it lands on top of what
 * overrides wrote; server-overrides has nothing to do on the client. */
static gboolean
extract_overrides (const char *archive_path,
                   const char *directory,
                   GError **error)
{
  guint64 used = 0;

  return extract_layers (archive_path, directory, &used, error);
}

typedef struct {
  char *archive_path;
  char *directory;
  AmlTaskScope *scope;
} ExtractRun;

static void
extract_run_free (ExtractRun *run)
{
  g_free (run->archive_path);
  g_free (run->directory);
  g_free (run);
}

static void
extract_thread (GTask *task G_GNUC_UNUSED,
                gpointer source_object G_GNUC_UNUSED,
                gpointer task_data, GCancellable *cancellable)
{
  ExtractRun *run = task_data;
  g_autoptr (GError) error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      aml_task_scope_return_error (
        run->scope,
        g_error_new_literal (AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_CANCELLED, "cancelled"));
      return;
    }

  if (!extract_overrides (run->archive_path, run->directory, &error))
    {
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      return;
    }

  if (g_cancellable_is_cancelled (cancellable))
    aml_task_scope_return_error (
      run->scope,
      g_error_new_literal (AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_CANCELLED, "cancelled"));
  else
    aml_task_scope_return_boolean (run->scope, TRUE);
}

AmlTaskScope *
extract_overrides_async (const char *archive_path, const char *directory,
                         GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  ExtractRun *run = g_new0 (ExtractRun, 1);

  /* Borrowed: the extract is the task's own data and the scope owns that
     task. The caller keeps the operation alive until its callback. */
  run->scope = scope;
  run->archive_path = g_strdup (archive_path);
  run->directory = g_strdup (directory);
  g_task_set_task_data (task, run, (GDestroyNotify) extract_run_free);
  g_task_run_in_thread (task, extract_thread);

  return g_steal_pointer (&scope);
}

gboolean
extract_overrides_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
