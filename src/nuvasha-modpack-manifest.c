/* Parse modpack manifest JSON and file entries. */

#include "nuvasha-modpack.h"
#include "nuvasha-json.h"

#include <string.h>

#define nuvasha_MODPACK_FORMAT_VERSION 1

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
      g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                           nuvasha_MODPACK_ERROR_FORMAT,
                           "the index holds no JSON document");
      return NULL;
    }

  return json_node_copy (root);
}

static NuvashaModpackEnv
env_from_node (JsonNode *env_node, const char *name)
{
  char *value = nuvasha_json_string (env_node, name);
  NuvashaModpackEnv env = nuvasha_MODPACK_ENV_UNSPECIFIED;

  if (value == NULL)
    return env;

  if (g_str_equal (value, "required"))
    env = nuvasha_MODPACK_ENV_REQUIRED;
  else if (g_str_equal (value, "optional"))
    env = nuvasha_MODPACK_ENV_OPTIONAL;
  else if (g_str_equal (value, "unsupported"))
    env = nuvasha_MODPACK_ENV_UNSUPPORTED;

  g_free (value);

  return env;
}

static NuvashaModpackFile *
file_from_node (JsonNode *node, GError **error)
{
  g_autoptr (NuvashaModpackFile) file = g_new0 (NuvashaModpackFile, 1);
  g_autofree char *path = nuvasha_json_string (node, "path");
  g_autofree char *sha1 = NULL;
  g_autofree char *sha512 = NULL;
  JsonNode *hashes = NULL;
  JsonNode *downloads_node = NULL;
  JsonObject *object = NULL;

  object = nuvasha_json_object_of (node);

  if (object == NULL)
    {
      g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                           nuvasha_MODPACK_ERROR_FORMAT,
                           "an entry of files is not an object");
      return NULL;
    }

  if (path == NULL || *path == '\0')
    {
      g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                           nuvasha_MODPACK_ERROR_FORMAT,
                           "an entry of files has no path");
      return NULL;
    }

  if (!nuvasha_modpack_path_is_safe (path))
    {
      g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_PATH,
                   "the manifest wants \"%s\", which is not a path the "
                   "instance can hold",
                   path);
      return NULL;
    }

  hashes = json_object_get_member (object, "hashes");

  if (hashes == NULL || !JSON_NODE_HOLDS_OBJECT (hashes))
    {
      g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_FORMAT,
                   "\"%s\" has no hashes object", path);
      return NULL;
    }

  sha1 = nuvasha_json_string (hashes, "sha1");
  sha512 = nuvasha_json_string (hashes, "sha512");

  if (sha1 == NULL || sha512 == NULL)
    {
      g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_FORMAT,
                   "\"%s\" misses the two hashes the format requires", path);
      return NULL;
    }

  downloads_node = json_object_get_member (object, "downloads");

  if (downloads_node == NULL || !JSON_NODE_HOLDS_ARRAY (downloads_node))
    {
      g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_FORMAT,
                   "\"%s\" has no downloads array", path);
      return NULL;
    }

  {
    JsonArray *downloads = json_node_get_array (downloads_node);

    file->downloads = g_ptr_array_new_with_free_func (g_free);

    if (json_array_get_length (downloads) == 0)
      {
        g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_FORMAT,
                     "\"%s\" has no download URL", path);
        return NULL;
      }

    for (guint i = 0; i < json_array_get_length (downloads); i++)
      {
        g_autofree char *url =
          json_node_dup_string (json_array_get_element (downloads, i));

        if (url == NULL || *url == '\0')
          {
            g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_FORMAT,
                         "\"%s\" has an empty download URL", path);
            return NULL;
          }

        g_ptr_array_add (file->downloads, g_steal_pointer (&url));
      }
  }

  file->path = g_steal_pointer (&path);
  file->sha1 = g_steal_pointer (&sha1);
  file->sha512 = g_steal_pointer (&sha512);
  file->file_size = (goffset) nuvasha_json_int (node, "fileSize", 0);

  {
    JsonNode *env = json_object_get_member (object, "env");

    file->env_client = env_from_node (env, "client");
    file->env_server = env_from_node (env, "server");
  }

  return g_steal_pointer (&file);
}

NuvashaModpack *
nuvasha_modpack_new_from_json (const char *json, gssize length, GError **error)
{
  g_autoptr (NuvashaModpack) pack = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *game = NULL;
  gint64 format_version = 0;
  JsonObject *object = NULL;

  root = parse_json (json, length, error);

  if (root == NULL)
    return NULL;

  object = nuvasha_json_object_of (root);

  if (object == NULL)
    {
      g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                           nuvasha_MODPACK_ERROR_FORMAT,
                           "the index is not a JSON object");
      return NULL;
    }

  format_version = nuvasha_json_int (root, "formatVersion", -1);

  if (format_version != nuvasha_MODPACK_FORMAT_VERSION)
    {
      g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_FORMAT,
                   "the format version \"%ld\" is one this launcher does "
                   "not know",
                   (long) format_version);
      return NULL;
    }

  game = nuvasha_json_string (root, "game");

  if (g_strcmp0 (game, "minecraft") != 0)
    {
      g_set_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_FORMAT,
                   "this launcher only installs Minecraft modpacks, and the "
                   "index holds \"%s\"",
                   game != NULL ? game : "(none)");
      return NULL;
    }

  pack = g_new0 (NuvashaModpack, 1);
  pack->format_version = (guint) format_version;
  pack->game = g_strdup (game);
  pack->version_id = nuvasha_json_string (root, "versionId");
  pack->name = nuvasha_json_string (root, "name");
  pack->summary = nuvasha_json_string (root, "summary");
  pack->files = g_ptr_array_new_with_free_func ((GDestroyNotify)
    nuvasha_modpack_file_free);
  pack->dependencies = g_ptr_array_new_with_free_func ((GDestroyNotify)
    nuvasha_modpack_dependency_free);
  pack->loader = nuvasha_LOADER_VANILLA;

  if (pack->name == NULL || *pack->name == '\0')
    {
      g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                           nuvasha_MODPACK_ERROR_FORMAT,
                            "the index has no name");
      return NULL;
    }

  {
    JsonNode *files_node = json_object_get_member (object, "files");

    if (files_node == NULL || !JSON_NODE_HOLDS_ARRAY (files_node))
      {
        g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                             nuvasha_MODPACK_ERROR_FORMAT,
                             "the index has no files array");
        return NULL;
      }

    JsonArray *files = json_node_get_array (files_node);

    for (guint i = 0; i < json_array_get_length (files); i++)
      {
        NuvashaModpackFile *file =
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
        g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                             nuvasha_MODPACK_ERROR_FORMAT,
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
            g_autoptr (NuvashaModpackDependency) dep =
              g_new0 (NuvashaModpackDependency, 1);

            dep->name = g_strdup (member_name);

            if (member_value != NULL &&
                JSON_NODE_HOLDS_VALUE (member_value) &&
                json_node_get_value_type (member_value) == G_TYPE_STRING)
              dep->version = json_node_dup_string (member_value);

            if (dep->version == NULL || *dep->version == '\0')
              {
                g_set_error (error, nuvasha_MODPACK_ERROR,
                             nuvasha_MODPACK_ERROR_FORMAT,
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
dependency_version (NuvashaModpack *modpack, const char *name)
{
  for (guint i = 0; i < modpack->dependencies->len; i++)
    {
      NuvashaModpackDependency *dependency =
        g_ptr_array_index (modpack->dependencies, i);

      if (g_str_equal (dependency->name, name))
        return dependency->version;
    }

  return NULL;
}

gboolean
nuvasha_modpack_resolve (NuvashaModpack *modpack, GError **error)
{
  static const struct {
    const char *name;
    NuvashaLoader loader;
  } loaders[] = {
    { "fabric-loader", nuvasha_LOADER_FABRIC },
    { "quilt-loader", nuvasha_LOADER_QUILT },
    { "forge", nuvasha_LOADER_FORGE },
    { "neoforge", nuvasha_LOADER_NEOFORGE },
  };

  const char *minecraft = NULL;

  g_return_val_if_fail (modpack != NULL, FALSE);

  minecraft = dependency_version (modpack, "minecraft");

  if (minecraft == NULL || *minecraft == '\0')
    {
      g_set_error_literal (error, nuvasha_MODPACK_ERROR,
                           nuvasha_MODPACK_ERROR_LOADER,
                           "the pack names no Minecraft version");
      return FALSE;
    }

  modpack->minecraft_version = g_strdup (minecraft);
  modpack->loader = nuvasha_LOADER_VANILLA;
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
nuvasha_modpack_required_client_files (NuvashaModpack *modpack)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (NULL);

  g_return_val_if_fail (modpack != NULL, files);

  for (guint i = 0; i < modpack->files->len; i++)
    {
      NuvashaModpackFile *file = g_ptr_array_index (modpack->files, i);

      /* A file with no env object belongs to what both sides can run:
       * the client can run everything it is not told to refuse. */
      if (file->env_client == nuvasha_MODPACK_ENV_UNSUPPORTED ||
          file->env_client == nuvasha_MODPACK_ENV_OPTIONAL)
        continue;

      g_ptr_array_add (files, file);
    }

  return files;
}

GPtrArray *
nuvasha_modpack_optional_client_files (NuvashaModpack *modpack)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (NULL);

  g_return_val_if_fail (modpack != NULL, files);

  for (guint i = 0; i < modpack->files->len; i++)
    {
      NuvashaModpackFile *file = g_ptr_array_index (modpack->files, i);

      if (file->env_client == nuvasha_MODPACK_ENV_OPTIONAL)
        g_ptr_array_add (files, file);
    }

  return files;
}
