/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlModpack: reading and safely installing Modrinth modpacks.
 *
 * The manifest (modrinth.index.json) is what defines the pack. Every
 * file the pack installs is downloaded from the hosts the format
 * names, and their hashes are checked against what the manifest says
 * before anything of the unpacked overrides is laid over it.
 *
 * The archive is made to look at security before it is trusted: the
 * entries of the zip are read through first, and an entry that names
 * a path outside the instance is refused, as is any link, any byte of
 * a bomb, and any URL from a host that is not the ones the format
 * has. */

#define _GNU_SOURCE

#include "aml-modpack.h"
#include "aml-i18n.h"

#include "aml-loaders.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

static JsonObject *
object_of (JsonNode *node)
{
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return NULL;

  return json_node_get_object (node);
}

/* The string @name of the object @node, or NULL. A field that is
 * there but is not a string counts as absent. */
static char *
object_string (JsonNode *node, const char *name)
{
  JsonObject *object = object_of (node);
  JsonNode *member = NULL;

  if (object == NULL)
    return NULL;

  member = json_object_get_member (object, name);

  if (member == NULL || !JSON_NODE_HOLDS_VALUE (member) ||
      json_node_get_value_type (member) != G_TYPE_STRING)
    return NULL;

  return json_node_dup_string (member);
}

/* Integer member, or @fallback for everything else. */
static gint64
object_int (JsonNode *node, const char *name, gint64 fallback)
{
  JsonObject *object = object_of (node);
  JsonNode *member = NULL;

  if (object == NULL)
    return fallback;

  member = json_object_get_member (object, name);

  if (member == NULL || !JSON_NODE_HOLDS_VALUE (member))
    return fallback;

  if (json_node_get_value_type (member) != G_TYPE_INT64 &&
      json_node_get_value_type (member) != G_TYPE_DOUBLE)
    return fallback;

  return (gint64) json_node_get_int (member);
}

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
  char *value = object_string (env_node, name);
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
  g_autofree char *path = object_string (node, "path");
  g_autofree char *sha1 = NULL;
  g_autofree char *sha512 = NULL;
  JsonNode *hashes = NULL;
  JsonNode *downloads_node = NULL;
  JsonObject *object = NULL;

  object = object_of (node);

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

  sha1 = object_string (hashes, "sha1");
  sha512 = object_string (hashes, "sha512");

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
  file->file_size = (goffset) object_int (node, "fileSize", 0);

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

  object = object_of (root);

  if (object == NULL)
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
                           "the index is not a JSON object");
      return NULL;
    }

  format_version = object_int (root, "formatVersion", -1);

  if (format_version != AML_MODPACK_FORMAT_VERSION)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "the format version \"%ld\" is one this launcher does "
                   "not know",
                   (long) format_version);
      return NULL;
    }

  game = object_string (root, "game");

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
  pack->version_id = object_string (root, "versionId");
  pack->name = object_string (root, "name");
  pack->summary = object_string (root, "summary");
  pack->files = g_ptr_array_new_with_free_func ((GDestroyNotify)
    aml_modpack_file_free);
  pack->dependencies = g_ptr_array_new_with_free_func ((GDestroyNotify)
    aml_modpack_dependency_free);
  pack->loader = AML_LOADER_VANILLA;

  if (pack->version_id == NULL || pack->name == NULL ||
      *pack->version_id == '\0' || *pack->name == '\0')
    {
      g_set_error_literal (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_FORMAT,
                           "the index has no version id or name");
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
} ModpackRead;

static void
modpack_read_free (ModpackRead *read)
{
  g_free (read->path);
  g_free (read);
}

static void
modpack_read_thread (GTask *task, gpointer source_object G_GNUC_UNUSED,
                     gpointer task_data, GCancellable *cancellable)
{
  ModpackRead *read = task_data;
  g_autoptr (GError) error = NULL;
  AmlModpack *pack = aml_modpack_new_from_zip (read->path, &error);

  if (g_cancellable_is_cancelled (cancellable))
    {
      aml_modpack_free (pack);
      g_task_return_new_error (task, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_CANCELLED, "cancelled");
      return;
    }

  if (pack == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, pack,
                         (GDestroyNotify) aml_modpack_free);
}

void
aml_modpack_new_from_zip_async (const char *zip_path,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_autoptr (GTask) task = NULL;
  ModpackRead *read = NULL;

  g_return_if_fail (zip_path != NULL);

  task = g_task_new (NULL, cancellable, callback, user_data);
  read = g_new0 (ModpackRead, 1);
  read->path = g_strdup (zip_path);
  g_task_set_task_data (task, read, (GDestroyNotify) modpack_read_free);
  g_task_run_in_thread (task, modpack_read_thread);
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
  GCancellable *cancellable;
  guint stage; /* 0: checking sha1, 1: checking sha512 */
  GTask *task; /* owned */
} VerifyRun;

static void
verify_run_free (VerifyRun *run)
{
  if (run == NULL)
    return;

  g_free (run->path);
  g_free (run->sha1);
  g_free (run->sha512);
  g_clear_object (&run->cancellable);
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
      g_task_return_error (run->task, g_steal_pointer (&error));
      verify_run_free (run);
      return;
    }

  expect = run->stage == 0 ? run->sha1 : run->sha512;

  if (g_ascii_strcasecmp (expect, digest) != 0)
    {
      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
                   "\"%s\" arrived with a different %s than the index says",
                   run->path, run->stage == 0 ? "sha1" : "sha512");
      g_task_return_error (run->task, g_steal_pointer (&error));
      verify_run_free (run);
      return;
    }

  run->stage++;

  if (run->stage > 1)
    {
      g_task_return_boolean (run->task, TRUE);
      verify_run_free (run);
      return;
    }

  {
    g_autoptr (GFile) file = g_file_new_for_path (run->path);

    aml_download_file_hash_async (file, G_CHECKSUM_SHA512, run->cancellable,
                                  verify_hash_ready, run);
  }
}

/* Both hashes of the manifest, out of the bytes on the disk. */
void
aml_modpack_verify_file_async (const AmlModpackFile *file,
                               const char *path,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_autoptr (GFile) job_file = g_file_new_for_path (path);
  VerifyRun *run = NULL;

  g_return_if_fail (file != NULL);
  g_return_if_fail (path != NULL);

  if (file->sha1 == NULL || file->sha512 == NULL)
    {
      g_task_return_new_error (
        task, AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
        "\"%s\" does not carry the two hashes the format requires",
        file->path != NULL ? file->path : "file");
      return;
    }

  run = g_new0 (VerifyRun, 1);
  run->path = g_strdup (path);
  run->sha1 = g_strdup (file->sha1);
  run->sha512 = g_strdup (file->sha512);
  run->cancellable =
    cancellable != NULL ? g_object_ref (cancellable) : NULL;
  run->stage = 0;
  run->task = g_object_ref (task);

  aml_download_file_hash_async (job_file, G_CHECKSUM_SHA1, run->cancellable,
                                verify_hash_ready, run);
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
extract_entry (struct archive *archive,
               struct archive_entry *header,
               const char *directory,
               const char *prefix,
               guint64 *used,
               GError **error)
{
  const char *name = archive_entry_pathname (header);
  g_autofree char *relative = NULL;
  g_autofree char *dest = NULL;
  g_autofree char *parent = NULL;
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

  dest = g_build_filename (directory, relative, NULL);

  if (archive_entry_filetype (header) == AE_IFDIR)
    {
      if (g_mkdir_with_parents (dest, 0755) != 0)
        {
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                       "\"%s\" cannot be made: %s", name,
                       g_strerror (errno));
          return FALSE;
        }

      return TRUE;
    }

  if (archive_entry_filetype (header) != AE_IFREG)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                   "\"%s\" is not a file", name);
      return FALSE;
    }

  parent = g_path_get_dirname (dest);

  if (g_mkdir_with_parents (parent, 0755) != 0)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "\"%s\" cannot be made: %s", name, g_strerror (errno));
      return FALSE;
    }

  out = g_fopen (dest, "wb");

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
          g_unlink (dest);
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
              g_unlink (dest);
              g_set_error_literal (error, AML_MODPACK_ERROR,
                                   AML_MODPACK_ERROR_BOMB,
                                   "the overrides decode past the size one "
                                   "pack carries");
              return FALSE;
            }

          if (fwrite (block, 1, size, out) != size)
            {
              fclose (out);
              g_unlink (dest);
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
      g_unlink (dest);
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "\"%s\" cannot be closed: %s", name, g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

/* One archive read for one of the two folder layers of a pack. */
static gboolean
extract_layer (const char *archive_path,
               const char *directory,
               const char *prefix,
               guint64 *used,
               GError **error)
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

      if (!extract_entry (archive, header, directory, prefix, used, error))
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
  static const char *prefixes[] = {
    "overrides/", "client-overrides/", "icon.png"
  };
  guint64 used = 0;

  for (guint i = 0; i < G_N_ELEMENTS (prefixes); i++)
    {
      if (!extract_layer (archive_path, directory, prefixes[i], &used,
                          error))
        return FALSE;
    }

  return TRUE;
}

/* ---------------------------------------------------------- *
 * The tree one install makes, to leave nothing of on failure
 * ---------------------------------------------------------- */

static void
remove_tree (const char *path)
{
  GDir *dir = NULL;
  const char *entry = NULL;

  /* A symlink has nothing of a tree under it for this walk: it goes as
   * it is, never followed. */
  if (g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    {
      g_remove (path);
      return;
    }

  dir = g_dir_open (path, 0, NULL);

  if (dir == NULL)
    {
      g_remove (path);
      return;
    }

  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *child = g_build_filename (path, entry, NULL);

      if (g_file_test (child, G_FILE_TEST_IS_DIR) &&
          !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        remove_tree (child);
      else
        g_remove (child);
    }

  g_dir_close (dir);
  g_rmdir (path);
}

/* ---------------------------------------------------------- *
 * One loader version against what the pack asks for
 * ---------------------------------------------------------- */

/* The names a loader publishes for its installer carry the game first
 * ("1.20.1-47.2.0"), and a pack names the own part only ("47.2.0").
 * Fabric and Quilt publish no installer, and their versions are bare
 * numbers either way. The two spellings are the same version. */
static gboolean
loader_version_matches (AmlLoader loader,
                        const char *have,
                        const char *want,
                        const char *game_version)
{
  if (g_strcmp0 (have, want) == 0)
    return TRUE;

  if (game_version != NULL && aml_loader_publishes_an_installer (loader))
    {
      const char separator = loader == AML_LOADER_FORGE ? '-' : '.';
      g_autofree char *prefix =
        g_strdup_printf ("%s%c", game_version, separator);

      if (g_str_has_prefix (have, prefix) &&
          g_strcmp0 (have + strlen (prefix), want) == 0)
        return TRUE;

      if (g_str_has_prefix (want, prefix) &&
          g_strcmp0 (want + strlen (prefix), have) == 0)
        return TRUE;
    }

  return FALSE;
}

/* ---------------------------------------------------------- *
 * The flow of one install
 * ---------------------------------------------------------- */

typedef struct
{
  AmlModpack *pack;     /* borrowed: alive until the last finish */
  AmlInstanceManager *manager;
  char *name;
  char *target;
  char *temp;
  GCancellable *cancellable;
  GPtrArray *files; /* merged required + chosen optional, borrowed */
  guint total;
  guint current;
  guint attempt;       /* which URL of the current file */
  gboolean saw_disallowed;
  gboolean saw_download_error;
  gboolean saw_hash_error;
  gboolean moved;
  AmlModpackProgressFunc progress;
  gpointer progress_data; /* user_data both for the result and progress */
  GTask *task;
} InstallRun;

static void install_next (InstallRun *run);
static void dispatch_download (InstallRun *run);
static void install_file_done (GObject *, GAsyncResult *, gpointer);
static void install_file_verified (GObject *, GAsyncResult *, gpointer);
static void install_loader_versions (GObject *, GAsyncResult *, gpointer);
static void install_register (InstallRun *run);

static void
install_run_free (InstallRun *run)
{
  if (run == NULL)
    return;

  g_ptr_array_unref (run->files); /* elements stay with the pack */
  g_clear_object (&run->manager);
  g_clear_object (&run->cancellable);
  g_clear_object (&run->task);
  g_free (run->name);
  g_free (run->target);
  g_free (run->temp);
  g_free (run);
}

/* Everything of ours goes away; anything that was on disk before the
 * install is never touched. */
static void
install_fail (InstallRun *run, GError *error)
{
  if (run->temp != NULL)
    remove_tree (run->temp);

  if (run->moved)
    remove_tree (run->target);

  g_task_return_error (run->task, error);
  install_run_free (run);
}

/* One of our temp validation went wrong, in front of any transfer: the
 * only message is a mistake of the caller, and nothing of a tree is
 * touched because none of it is made. */
static void
install_refuse (GTask *task, InstallRun *run, GError *error)
{
  g_task_return_error (task, error);
  install_run_free (run);
}

static void
install_loader_versions (GObject *source G_GNUC_UNUSED,
                         GAsyncResult *result,
                         gpointer user_data);
static void install_register (InstallRun *run);

static void
install_after_overrides (InstallRun *run)
{
  AmlModpack *pack = run->pack;

  if (pack->loader == AML_LOADER_VANILLA)
    {
      install_register (run);
      return;
    }

  aml_loaders_versions_async (pack->loader, pack->minecraft_version,
                              run->cancellable, install_loader_versions, run);
}

static void
install_loader_versions (GObject *source G_GNUC_UNUSED,
                         GAsyncResult *result,
                         gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  AmlModpack *pack = run->pack;
  gboolean found = FALSE;

  versions = aml_loaders_versions_finish (result, &error);

  if (versions == NULL)
    {
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_clear_error (&error);
          error = g_error_new_literal (AML_MODPACK_ERROR,
                                       AML_MODPACK_ERROR_CANCELLED,
                                       "cancelled");
        }
      else
        {
          g_autofree char *message =
             g_strdup_printf (_("the loader cannot be resolved: %s"),
                             error->message);

          g_clear_error (&error);
          error = g_error_new_literal (AML_MODPACK_ERROR,
                                       AML_MODPACK_ERROR_LOADER, message);
        }

      install_fail (run, g_steal_pointer (&error));
      return;
    }

  for (guint i = 0; i < versions->len && !found; i++)
    {
      AmlLoaderVersion *version = g_ptr_array_index (versions, i);

      if (loader_version_matches (pack->loader, version->version,
                                  pack->loader_version, pack->minecraft_version))
        found = TRUE;
    }

  if (!found)
    {
      g_autofree char *message = g_strdup_printf (
        "\"%s\" names \"%s\", and no such version of it runs Minecraft %s",
        pack->name != NULL ? pack->name : "the pack", pack->loader_version,
        pack->minecraft_version != NULL ? pack->minecraft_version : "?");

      g_set_error_literal (&error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_LOADER, message);
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  install_register (run);
}

static void
install_register (InstallRun *run)
{
  g_autoptr (AmlInstance) instance = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *temp = NULL;
  AmlModpack *pack = run->pack;

  /* Everything of the pack is in its temp directory: rename is the
   * atomic set-in-place, and there is nothing of ours next to the
   * target but that same tree it just became. The temp directory is
   * no longer ours to clean up on a failure once the move is done, so
   * run->temp is emptied as the move starts. */
  temp = g_steal_pointer (&run->temp);

  if (g_rename (temp, run->target) != 0)
    {
      run->temp = g_steal_pointer (&temp);
      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "the unpacked pack cannot be moved: %s",
                   g_strerror (errno));
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  run->moved = TRUE;

  instance = aml_instance_manager_create (run->manager, run->name,
                                          run->target, pack->minecraft_version,
                                          pack->loader, pack->loader_version,
                                          &error);

  if (instance == NULL)
    {
      remove_tree (run->target);
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  if (pack->icon != NULL)
    {
      g_autofree char *icon = g_build_filename (run->target, pack->icon, NULL);

      if (g_file_test (icon, G_FILE_TEST_IS_REGULAR))
        {
          g_object_set (instance, "icon", icon, NULL);
          if (!aml_instance_manager_save (run->manager, &error))
            {
              remove_tree (run->target);
              install_fail (run, g_steal_pointer (&error));
              return;
            }
        }
    }

  g_task_return_pointer (run->task, g_object_ref (instance),
                         g_object_unref);
  install_run_free (run);
}

/* The next file in line, or the overrides once there are none. */
static void
install_next (InstallRun *run)
{
  if (run->current >= run->total)
    {
      g_autoptr (GError) error = NULL;

      if (!extract_overrides (run->pack->archive_path, run->temp, &error))
        {
          install_fail (run, g_steal_pointer (&error));
          return;
        }

      install_after_overrides (run);
      return;
    }

  run->attempt = 0;
  run->saw_disallowed = FALSE;
  run->saw_download_error = FALSE;
  run->saw_hash_error = FALSE;

  dispatch_download (run);
}

static void
dispatch_download (InstallRun *run)
{
  AmlModpackFile *file = g_ptr_array_index (run->files, run->current);
  const char *url = NULL;

  while (run->attempt < file->downloads->len)
    {
      url = g_ptr_array_index (file->downloads, run->attempt);

      if (aml_modpack_download_url_allowed (url))
        break;

      run->saw_disallowed = TRUE;
      run->attempt++;
    }

  if (run->attempt >= file->downloads->len)
    {
      g_autoptr (GError) error = NULL;

      if (run->saw_hash_error)
        {
          g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
                       "what arrived for \"%s\" did not match the hashes of "
                       "the manifest",
                       file->path);
        }
      else if (run->saw_download_error)
        {
          g_set_error (&error, AML_MODPACK_ERROR,
                       AML_MODPACK_ERROR_DOWNLOAD,
                       "\"%s\" could not be downloaded", file->path);
        }
      else if (run->saw_disallowed)
        {
          g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_URL,
                       "every URL the manifest gives for \"%s\" is from a "
                       "host this launcher cannot ask",
                       file->path);
        }
      else
        {
          g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_DOWNLOAD,
                       "\"%s\" has no download URL", file->path);
        }

      install_fail (run, g_steal_pointer (&error));
      return;
    }

  if (!aml_modpack_path_is_safe (file->path))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
                   "the manifest wants \"%s\", which is not a path the "
                   "instance can hold",
                   file->path);
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  {
    g_autofree char *dest = g_build_filename (run->temp, file->path, NULL);
    g_autofree char *parent = g_path_get_dirname (dest);

    if (g_mkdir_with_parents (parent, 0755) != 0)
      {
        g_autoptr (GError) error = NULL;

        g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                     "a directory under the instance cannot be made: %s",
                     g_strerror (errno));
        install_fail (run, g_steal_pointer (&error));
        return;
      }

    aml_download_file_async (url, dest, NULL, 0, run->cancellable, NULL,
                             install_file_done, run);
  }
}

static void
install_file_done (GObject *source G_GNUC_UNUSED,
                   GAsyncResult *result,
                   gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_download_file_finish (result, &error))
    {
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          install_fail (
            run,
            g_error_new_literal (AML_MODPACK_ERROR,
                                 AML_MODPACK_ERROR_CANCELLED, "cancelled"));
          return;
        }

      run->saw_download_error = TRUE;

      /* The next URL of the same file may work where the last did not. */
      run->attempt++;
      dispatch_download (run);
      return;
    }

  {
    AmlModpackFile *file = g_ptr_array_index (run->files, run->current);
    g_autofree char *dest = g_build_filename (run->temp, file->path, NULL);

    aml_modpack_verify_file_async (file, dest, run->cancellable,
                                   install_file_verified, run);
  }
}

static void
install_file_verified (GObject *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;
  AmlModpackFile *file = NULL;
  g_autofree char *dest = NULL;

  if (!aml_modpack_verify_file_finish (result, &error))
    {
      /* A hash that does not match is no good download: wrong in a way
       * a try of the next URL may put right. */
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          install_fail (
            run,
            g_error_new_literal (AML_MODPACK_ERROR,
                                 AML_MODPACK_ERROR_CANCELLED, "cancelled"));
          return;
        }

      file = g_ptr_array_index (run->files, run->current);
      dest = g_build_filename (run->temp, file->path, NULL);

      g_unlink (dest);

      run->saw_hash_error = TRUE;
      run->attempt++;

      dispatch_download (run);
      return;
    }

  file = g_ptr_array_index (run->files, run->current);

  /* The file that arrived has the two hashes: the next file, and what
   * of this one the user sees. */
  run->current++;

  if (run->progress != NULL)
    run->progress (run->current, run->total, file->path, run->progress_data);

  install_next (run);
}

void
aml_modpack_install_async (AmlModpack *modpack,
                           AmlInstanceManager *manager,
                           const char *name,
                           const char *directory,
                           const GPtrArray *optional_files,
                           GCancellable *cancellable,
                           AmlModpackProgressFunc progress,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  InstallRun *run = NULL;
  g_autofree char *parent = NULL;
  g_autofree char *template = NULL;

  if (modpack == NULL || manager == NULL || name == NULL || name[0] == '\0')
    {
      g_task_return_new_error (task, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_FORMAT,
                               "the install is missing its modpack, its "
                               "manager or its name");
      return;
    }

  if (modpack->minecraft_version == NULL)
    {
      g_task_return_new_error (task, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_LOADER,
                               "the dependencies were never read: call "
                               "aml_modpack_resolve() first");
      return;
    }

  run = g_new0 (InstallRun, 1);

  run->pack = modpack;
  run->manager = g_object_ref (manager);
  run->name = g_strdup (name);
  run->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  run->progress = progress;
  run->progress_data = user_data;
  run->files = g_ptr_array_new ();
  run->task = g_object_ref (task);

  /* The target is what the manager would choose for the name, unless
   * the caller says where the instance is to go. */
  if (directory != NULL)
    run->target = g_strdup (directory);
  else
    run->target = aml_instance_manager_default_directory (manager, name);

  if (run->target == NULL || !g_path_is_absolute (run->target))
    {
      g_autoptr (GError) error = NULL;

      g_set_error_literal (&error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_IO,
                           "the directory of the instance is not usable");
      install_refuse (task, run, g_steal_pointer (&error));
      return;
    }

  /* What already runs in the chosen place is user data: never touch it. */
  if (g_file_test (run->target, G_FILE_TEST_EXISTS))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "a directory already exists at \"%s\"", run->target);
      install_refuse (task, run, g_steal_pointer (&error));
      return;
    }

  {
    g_autoptr (GPtrArray) required =
      aml_modpack_required_client_files (modpack);

    for (guint i = 0; i < required->len; i++)
      g_ptr_array_add (run->files, g_ptr_array_index (required, i));

    if (optional_files != NULL)
      {
        for (guint i = 0; i < optional_files->len; i++)
          g_ptr_array_add (run->files, g_ptr_array_index (optional_files, i));
      }
  }

  run->total = run->files->len;
  run->current = 0;
  run->attempt = 0;

  /* Room for the tree the install is built in, next to where it ends
   * up: the final move is one rename on one filesystem. The parent is
   * made if it has to be; what is made stays. */
  parent = g_path_get_dirname (run->target);

  if (g_mkdir_with_parents (parent, 0755) != 0)
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "a directory under the instance cannot be made: %s",
                   g_strerror (errno));
      install_refuse (task, run, g_steal_pointer (&error));
      return;
    }

  template = g_strdup_printf ("%s/.aml-modpack-XXXXXX", parent);
  run->temp = g_mkdtemp (template);

  if (run->temp == NULL)
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "the temporary directory cannot be made: %s",
                   g_strerror (errno));
      install_refuse (task, run, g_steal_pointer (&error));
      return;
    }

  /* g_mkdtemp fills the template in place and hands back that very
   * pointer: run->temp is its owner now and the local name has
   * nothing left to free. */
  template = NULL;

  /* install_next and not dispatch_download: a pack may name no files
   * at all to download, and then the first thing it wants is its
   * overrides, not a file that is not there. */
  install_next (run);
}

AmlInstance *
aml_modpack_install_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

typedef struct {
  char *instance_directory;
  char *archive_path;
  char *manifest;
  char *icon_path;
  GHashTable *indexed_paths;
} ExportRun;

static void
export_run_free (ExportRun *run)
{
  g_free (run->instance_directory);
  g_free (run->archive_path);
  g_free (run->manifest);
  g_free (run->icon_path);
  g_clear_pointer (&run->indexed_paths, g_hash_table_unref);
  g_free (run);
}

static gboolean
export_file (struct archive *archive, const char *source, const char *target,
             GError **error)
{
  GStatBuf stat_buf;
  FILE *input = NULL;
  struct archive_entry *entry = NULL;
  char buffer[64 * 1024];
  size_t length;

  if (g_stat (source, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "cannot read instance file \"%s\": %s", source,
                   g_strerror (errno));
      return FALSE;
    }

  input = g_fopen (source, "rb");
  if (input == NULL)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "cannot open instance file \"%s\": %s", source,
                   g_strerror (errno));
      return FALSE;
    }

  entry = archive_entry_new ();
  archive_entry_set_pathname (entry, target);
  archive_entry_set_size (entry, stat_buf.st_size);
  archive_entry_set_filetype (entry, AE_IFREG);
  archive_entry_set_perm (entry, 0644);

  if (archive_write_header (archive, entry) != ARCHIVE_OK)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "cannot write \"%s\": %s", target,
                   archive_error_string (archive));
      archive_entry_free (entry);
      fclose (input);
      return FALSE;
    }

  while ((length = fread (buffer, 1, sizeof buffer, input)) > 0)
    if (archive_write_data (archive, buffer, length) != (la_ssize_t) length)
      {
        g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                     "cannot write \"%s\": %s", target,
                     archive_error_string (archive));
        archive_entry_free (entry);
        fclose (input);
        return FALSE;
      }

  if (ferror (input))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "cannot read instance file \"%s\": %s", source,
                   g_strerror (errno));
      archive_entry_free (entry);
      fclose (input);
      return FALSE;
    }

  fclose (input);
  archive_entry_free (entry);
  return TRUE;
}

static gboolean
export_directory (struct archive *archive, const char *root,
                  const char *relative, GHashTable *indexed_paths,
                  GError **error)
{
  g_autofree char *directory = g_build_filename (root, relative, NULL);
  g_autoptr (GDir) dir = g_dir_open (directory, 0, error);
  const char *name = NULL;

  if (dir == NULL)
    return FALSE;

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *source = g_build_filename (directory, name, NULL);
      g_autofree char *child_relative =
        g_build_filename (relative, name, NULL);
      GStatBuf stat_buf;

      if (g_lstat (source, &stat_buf) != 0)
        {
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                       "cannot inspect instance file \"%s\": %s", source,
                       g_strerror (errno));
          return FALSE;
        }

      if (S_ISDIR (stat_buf.st_mode))
        {
          if (!export_directory (archive, root, child_relative,
                                 indexed_paths, error))
            return FALSE;
        }
      else if (S_ISREG (stat_buf.st_mode))
        {
          g_autofree char *target =
            g_build_filename ("overrides", child_relative, NULL);

          if (g_hash_table_contains (indexed_paths, child_relative))
            continue;

          if (!export_file (archive, source, target, error))
            return FALSE;
        }
      else if (S_ISLNK (stat_buf.st_mode))
        {
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
                       "cannot export symlink \"%s\"", source);
          return FALSE;
        }
    }

  return TRUE;
}

/* Add installed content whose AML index has both hashes and its original
 * Modrinth URL. Files without complete metadata remain overrides, which is
 * still a valid and lossless export. */
static gboolean
export_indexed_files (ExportRun *run, GError **error)
{
  static const char *folders[] = { "mods", "resourcepacks", "shaderpacks" };
  g_autoptr (JsonParser) manifest_parser = json_parser_new ();
  JsonNode *manifest_root = NULL;
  JsonObject *manifest = NULL;
  JsonArray *files = NULL;

  if (!json_parser_load_from_data (manifest_parser, run->manifest, -1, error))
    return FALSE;

  manifest_root = json_parser_get_root (manifest_parser);
  manifest = json_node_get_object (manifest_root);
  files = json_object_get_array_member (manifest, "files");

  for (guint folder_index = 0; folder_index < G_N_ELEMENTS (folders);
       folder_index++)
    {
      g_autofree char *index_path = g_build_filename (
        run->instance_directory, folders[folder_index], ".index", "index.json",
        NULL);
      g_autofree char *data = NULL;
      gsize length = 0;
      g_autoptr (JsonParser) index_parser = json_parser_new ();
      JsonNode *index_root = NULL;
      JsonArray *entries = NULL;

      if (!g_file_get_contents (index_path, &data, &length, NULL) ||
          !json_parser_load_from_data (index_parser, data, length, NULL))
        continue;

      index_root = json_parser_get_root (index_parser);
      if (!JSON_NODE_HOLDS_OBJECT (index_root))
        continue;
      entries = json_object_get_array_member (json_node_get_object (index_root),
                                              "entries");
      if (entries == NULL)
        continue;

      for (guint i = 0; i < json_array_get_length (entries); i++)
        {
          JsonObject *entry = json_node_get_object (
            json_array_get_element (entries, i));
          const char *filename = NULL;
          const char *sha1 = NULL;
          const char *sha512 = NULL;
          const char *url = NULL;
          g_autofree char *source = NULL;
          GStatBuf stat_buf;
          JsonObject *file = NULL;
          JsonObject *hashes = NULL;
          JsonArray *downloads = NULL;

          if (entry == NULL)
            continue;
          filename = json_object_get_string_member_with_default (entry,
                                                                  "filename",
                                                                  "");
          sha1 = json_object_get_string_member_with_default (entry, "hash", "");
          sha512 = json_object_get_string_member_with_default (entry, "hash512",
                                                               "");
          url = json_object_get_string_member_with_default (entry,
                                                            "download_url", "");
          source = g_build_filename (run->instance_directory, folders[folder_index],
                                     filename, NULL);

          if (*filename == '\0' || *sha1 == '\0' || *sha512 == '\0' ||
              *url == '\0' || !aml_modpack_download_url_allowed (url) ||
              g_stat (source, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
            continue;

          g_autofree char *relative =
            g_build_filename (folders[folder_index], filename, NULL);

          file = json_object_new ();
          hashes = json_object_new ();
          downloads = json_array_new ();
          json_object_set_string_member (file, "path", relative);
          json_object_set_string_member (hashes, "sha1", sha1);
          json_object_set_string_member (hashes, "sha512", sha512);
          json_object_set_object_member (file, "hashes", hashes);
          json_array_add_string_element (downloads, url);
          json_object_set_array_member (file, "downloads", downloads);
          json_object_set_int_member (file, "fileSize", stat_buf.st_size);
          json_array_add_object_element (files, file);
          g_hash_table_add (run->indexed_paths,
                            g_build_filename (folders[folder_index], filename,
                                              NULL));
        }
    }

  {
    g_autoptr (JsonGenerator) generator = json_generator_new ();
    g_autofree char *data = NULL;

    json_generator_set_root (generator, manifest_root);
    data = json_generator_to_data (generator, NULL);
    g_free (run->manifest);
    run->manifest = g_steal_pointer (&data);
  }

  return TRUE;
}

static void
export_thread (GTask *task, gpointer source_object G_GNUC_UNUSED,
               gpointer task_data, GCancellable *cancellable)
{
  ExportRun *run = task_data;
  g_autoptr (GError) error = NULL;
  struct archive *archive = NULL;
  struct archive_entry *manifest_entry = NULL;
  gsize manifest_length = 0;
  g_autofree char *canonical_root =
    g_canonicalize_filename (run->instance_directory, NULL);
  g_autofree char *canonical_archive =
    g_canonicalize_filename (run->archive_path, NULL);
  g_autofree char *root_prefix =
    g_strconcat (canonical_root, G_DIR_SEPARATOR_S, NULL);

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_task_return_new_error (task, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_CANCELLED, "cancelled");
      return;
    }

  if (!export_indexed_files (run, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  manifest_length = strlen (run->manifest);

  if (g_str_has_prefix (canonical_archive, root_prefix))
    {
      g_task_return_new_error (task, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                               "the export file cannot be inside the instance");
      return;
    }

  archive = archive_write_new ();
  archive_write_set_format_zip (archive);

  if (archive_write_open_filename (archive, run->archive_path) != ARCHIVE_OK)
    {
      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "cannot create \"%s\": %s", run->archive_path,
                   archive_error_string (archive));
      archive_write_free (archive);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  manifest_entry = archive_entry_new ();
  archive_entry_set_pathname (manifest_entry, "modrinth.index.json");
  archive_entry_set_size (manifest_entry, manifest_length);
  archive_entry_set_filetype (manifest_entry, AE_IFREG);
  archive_entry_set_perm (manifest_entry, 0644);

  if (archive_write_header (archive, manifest_entry) != ARCHIVE_OK ||
      archive_write_data (archive, run->manifest, manifest_length) !=
        (la_ssize_t) manifest_length ||
      !export_directory (archive, canonical_root, "", run->indexed_paths,
                         &error) ||
      (run->icon_path != NULL &&
      !export_file (archive, run->icon_path, "overrides/icon.png",
                     &error)) ||
      g_cancellable_is_cancelled (cancellable) ||
      archive_write_close (archive) != ARCHIVE_OK)
    {
      if (error == NULL)
        g_set_error (&error, AML_MODPACK_ERROR,
                     g_cancellable_is_cancelled (cancellable)
                       ? AML_MODPACK_ERROR_CANCELLED
                       : AML_MODPACK_ERROR_ARCHIVE,
                     g_cancellable_is_cancelled (cancellable)
                       ? "cancelled" : "cannot finish the modpack archive");
      archive_entry_free (manifest_entry);
      archive_write_free (archive);
      g_unlink (run->archive_path);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  archive_entry_free (manifest_entry);
  archive_write_free (archive);
  g_task_return_boolean (task, TRUE);
}

static char *
export_manifest (AmlInstance *instance, GError **error)
{
  g_autofree char *name = NULL;
  g_autofree char *version = NULL;
  g_autofree char *loader_version = NULL;
  g_autofree char *icon = NULL;
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *result = NULL;
  AmlLoader loader = AML_LOADER_VANILLA;
  gsize length = 0;

  g_object_get (instance, "name", &name, "minecraft_version", &version,
                "loader", &loader, "loader_version", &loader_version,
                "icon", &icon, NULL);

  if (version == NULL || *version == '\0')
    {
      g_set_error_literal (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                           "the instance has no Minecraft version");
      return NULL;
    }

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "formatVersion");
  json_builder_add_int_value (builder, AML_MODPACK_FORMAT_VERSION);
  json_builder_set_member_name (builder, "game");
  json_builder_add_string_value (builder, "minecraft");
  json_builder_set_member_name (builder, "name");
  json_builder_add_string_value (builder, name != NULL ? name : "AML Instance");
  json_builder_set_member_name (builder, "versionId");
  json_builder_add_string_value (builder, "aml-export");
  json_builder_set_member_name (builder, "dependencies");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "minecraft");
  json_builder_add_string_value (builder, version);
  if (loader != AML_LOADER_VANILLA && loader_version != NULL)
    {
      const char *loader_name = aml_loader_to_string (loader);

      if (loader == AML_LOADER_FABRIC)
        loader_name = "fabric-loader";
      else if (loader == AML_LOADER_QUILT)
        loader_name = "quilt-loader";

      json_builder_set_member_name (builder, loader_name);
      json_builder_add_string_value (builder, loader_version);
    }
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "files");
  json_builder_begin_array (builder);
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  result = json_generator_to_data (generator, &length);
  return g_steal_pointer (&result);
}

void
aml_modpack_export_async (AmlInstance *instance, const char *path,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = NULL;
  ExportRun *run = NULL;

  g_return_if_fail (AML_IS_INSTANCE (instance));
  g_return_if_fail (path != NULL && g_path_is_absolute (path));

  task = g_task_new (NULL, cancellable, callback, user_data);
  run = g_new0 (ExportRun, 1);
  g_object_get (instance, "directory", &directory, NULL);
  run->instance_directory = g_strdup (directory);
  run->archive_path = g_strdup (path);
  run->indexed_paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              NULL);
  g_object_get (instance, "icon", &run->icon_path, NULL);
  if (run->icon_path != NULL &&
      !g_file_test (run->icon_path, G_FILE_TEST_IS_REGULAR))
    g_clear_pointer (&run->icon_path, g_free);
  run->manifest = export_manifest (instance, &error);

  if (run->instance_directory == NULL || run->manifest == NULL)
    {
      export_run_free (run);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_set_task_data (task, run, (GDestroyNotify) export_run_free);
  g_task_run_in_thread (task, export_thread);
}

gboolean
aml_modpack_export_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
