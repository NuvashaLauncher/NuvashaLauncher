/* Minecraft version-description JSON parsing. */

#include "nuvasha-minecraft-versions-json-private.h"

#include <string.h>

/* @url_required is FALSE for the places where Mojang gives a path but
 * no address, which the launcher fills in later. */
static NuvashaDownloadInfo *
parse_download_info (JsonObject *object, gboolean url_required, GError **error)
{
  NuvashaDownloadInfo *info = g_new0 (NuvashaDownloadInfo, 1);

  if (!nuvasha_mc_get_string (object, "url", url_required, &info->url, error) ||
      !nuvasha_mc_get_string (object, "sha1", FALSE, &info->sha1, error) ||
      !nuvasha_mc_get_string (object, "id", FALSE, &info->id, error) ||
      !nuvasha_mc_get_string (object, "path", FALSE, &info->path, error) ||
      !nuvasha_mc_get_int64 (object, "size", FALSE, &info->size, error) ||
      !nuvasha_mc_get_int64 (object, "totalSize", FALSE, &info->total_size, error))
    {
      nuvasha_mc_free_download_info (info);
      return NULL;
    }

  return info;
}

/* *value is NULL when the member is absent. */
static gboolean
get_download_member (JsonObject     *object,
                     const char     *member,
                     gboolean        url_required,
                     NuvashaDownloadInfo **value,
                     GError        **error)
{
  JsonObject *child = NULL;

  *value = NULL;

  if (!nuvasha_mc_get_object (object, member, FALSE, &child, error))
    return FALSE;

  if (child == NULL)
    return TRUE;

  *value = parse_download_info (child, url_required, error);
  return *value != NULL;
}

static NuvashaLibrary *
parse_library (JsonObject *object, GError **error)
{
  NuvashaLibrary *library = g_new0 (NuvashaLibrary, 1);
  JsonObject *downloads = NULL;
  JsonObject *classifiers = NULL;
  JsonObject *natives = NULL;
  JsonObject *extract = NULL;

  if (!nuvasha_mc_get_string (object, "name", TRUE, &library->name, error))
    goto fail;

  if (!nuvasha_mc_get_object (object, "downloads", FALSE, &downloads, error))
    goto fail;

  if (downloads != NULL)
    {
      if (!get_download_member (downloads, "artifact", FALSE,
                                &library->artifact, error))
        goto fail;

      if (!nuvasha_mc_get_object (downloads, "classifiers", FALSE, &classifiers, error))
        goto fail;

      if (classifiers != NULL)
        {
          JsonObjectIter iter;
          const char *name = NULL;
          JsonNode *node = NULL;

          library->classifiers =
            g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   nuvasha_mc_free_download_info);
          json_object_iter_init (&iter, classifiers);

          while (json_object_iter_next (&iter, &name, &node))
            {
              NuvashaDownloadInfo *info;

              if (!JSON_NODE_HOLDS_OBJECT (node))
                {
                  g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                               "classifier \"%s\" must be an object", name);
                  goto fail;
                }

              info = parse_download_info (json_node_get_object (node), FALSE,
                                          error);

              if (info == NULL)
                goto fail;

              g_hash_table_replace (library->classifiers, g_strdup (name),
                                    info);
            }
        }
    }

  if (!nuvasha_mc_get_object (object, "natives", FALSE, &natives, error))
    goto fail;

  if (natives != NULL)
    {
      JsonObjectIter iter;
      const char *name = NULL;
      JsonNode *node = NULL;

      library->natives = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
      json_object_iter_init (&iter, natives);

      while (json_object_iter_next (&iter, &name, &node))
        {
          if (!JSON_NODE_HOLDS_VALUE (node) ||
              json_node_get_value_type (node) != G_TYPE_STRING)
            {
              g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                           "native \"%s\" must be a string", name);
              goto fail;
            }

          g_hash_table_replace (library->natives, g_strdup (name),
                                g_strdup (json_node_get_string (node)));
        }
    }

  if (!nuvasha_mc_get_rules_member (object, "rules", &library->rules, error))
    goto fail;

  if (!nuvasha_mc_get_object (object, "extract", FALSE, &extract, error))
    goto fail;

  if (extract != NULL)
    {
      JsonArray *array = NULL;

      if (!nuvasha_mc_get_array (extract, "exclude", FALSE, &array, error))
        goto fail;

      if (array != NULL)
        {
          library->exclude = g_ptr_array_new_with_free_func (g_free);

          for (guint i = 0; i < json_array_get_length (array); i++)
            {
              JsonNode *item = json_array_get_element (array, i);

              if (!JSON_NODE_HOLDS_VALUE (item) ||
                  json_node_get_value_type (item) != G_TYPE_STRING)
                {
                  g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                               "exclude %u must be a string", i);
                  goto fail;
                }

              g_ptr_array_add (library->exclude,
                               g_strdup (json_node_get_string (item)));
            }
        }
    }

  return library;

fail:
  nuvasha_mc_free_library (library);
  return NULL;
}

/* One entry of arguments.jvm or arguments.game: either a plain string
 * or an object carrying rules and one or several values. */
static NuvashaArgument *
parse_argument (JsonNode *node, GError **error)
{
  NuvashaArgument *argument = g_new0 (NuvashaArgument, 1);

  argument->values = g_ptr_array_new_with_free_func (g_free);

  if (JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    {
      g_ptr_array_add (argument->values,
                       g_strdup (json_node_get_string (node)));
      return argument;
    }

  if (!JSON_NODE_HOLDS_OBJECT (node))
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "an argument must be a string or an object");
      goto fail;
    }

  {
    JsonObject *object = json_node_get_object (node);
    JsonNode *value = json_object_get_member (object, "value");

    if (!nuvasha_mc_get_rules_member (object, "rules", &argument->rules, error))
      goto fail;

    if (value == NULL || JSON_NODE_HOLDS_NULL (value))
      {
        g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_MISSING,
                     "required field \"value\" is missing");
        goto fail;
      }

    if (JSON_NODE_HOLDS_VALUE (value) &&
        json_node_get_value_type (value) == G_TYPE_STRING)
      {
        g_ptr_array_add (argument->values,
                         g_strdup (json_node_get_string (value)));
        return argument;
      }

    if (JSON_NODE_HOLDS_ARRAY (value))
      {
        JsonArray *array = json_node_get_array (value);

        for (guint i = 0; i < json_array_get_length (array); i++)
          {
            JsonNode *item = json_array_get_element (array, i);

            if (!JSON_NODE_HOLDS_VALUE (item) ||
                json_node_get_value_type (item) != G_TYPE_STRING)
              {
                g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                             "argument value %u must be a string", i);
                goto fail;
              }

            g_ptr_array_add (argument->values,
                             g_strdup (json_node_get_string (item)));
          }

        return argument;
      }

    g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                 "field \"value\" must be a string or an array");
  }

fail:
  nuvasha_mc_free_argument (argument);
  return NULL;
}

/* arguments.jvm and arguments.game. Mojang writes other groups beside
 * them, such as the default heap sizes, and they are not part of what
 * launches the game, so they are skipped. */
static gboolean
parse_argument_group (JsonObject   *object,
                      const char   *member,
                      GPtrArray   **value,
                      GError      **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = g_ptr_array_new_with_free_func (nuvasha_mc_free_argument);

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  if (!JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "field \"%s\" must be an array", member);
      return FALSE;
    }

  {
    JsonArray *array = json_node_get_array (node);

    for (guint i = 0; i < json_array_get_length (array); i++)
      {
        NuvashaArgument *argument =
          parse_argument (json_array_get_element (array, i), error);

        if (argument == NULL)
          return FALSE;

        g_ptr_array_add (*value, argument);
      }
  }

  return TRUE;
}

static gboolean
parse_arguments (JsonObject *root,
                 NuvashaVersionDescription *self,
                 GError **error)
{
  JsonObject *arguments = NULL;

  if (!nuvasha_mc_get_object (root, "arguments", FALSE, &arguments, error))
    return FALSE;

  if (arguments == NULL)
    {
      /* A legacy document: only minecraftArguments, and no modern
       * groups at all. */
      self->jvm_arguments = g_ptr_array_new_with_free_func (nuvasha_mc_free_argument);
      self->game_arguments = g_ptr_array_new_with_free_func (nuvasha_mc_free_argument);
      return TRUE;
    }

  self->modern_arguments = TRUE;

  if (!parse_argument_group (arguments, "jvm", &self->jvm_arguments, error))
    return FALSE;

  return parse_argument_group (arguments, "game", &self->game_arguments, error);
}

static gboolean
parse_libraries (JsonObject *root, GPtrArray **value, GError **error)
{
  JsonNode *node = json_object_get_member (root, "libraries");

  *value = g_ptr_array_new_with_free_func (nuvasha_mc_free_library);

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  if (!JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "field \"libraries\" must be an array");
      return FALSE;
    }

  {
    JsonArray *array = json_node_get_array (node);

    for (guint i = 0; i < json_array_get_length (array); i++)
      {
        JsonNode *item = json_array_get_element (array, i);
        NuvashaLibrary *library;

        if (!JSON_NODE_HOLDS_OBJECT (item))
          {
            g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                         "library %u is not an object", i);
            return FALSE;
          }

        library = parse_library (json_node_get_object (item), error);

        if (library == NULL)
          return FALSE;

        g_ptr_array_add (*value, library);
      }
  }

  return TRUE;
}

/* *logging is NULL when the document has nothing to say about the
 * log4j configuration. */
static gboolean
parse_logging (JsonObject *root, NuvashaLogging **logging, GError **error)
{
  JsonObject *entry = NULL;
  JsonObject *client = NULL;

  *logging = NULL;

  if (!nuvasha_mc_get_object (root, "logging", FALSE, &entry, error))
    return FALSE;

  if (entry == NULL)
    return TRUE;

  if (!nuvasha_mc_get_object (entry, "client", FALSE, &client, error))
    return FALSE;

  if (client == NULL)
    return TRUE;

  {
    NuvashaLogging *logging_client = g_new0 (NuvashaLogging, 1);

    if (!nuvasha_mc_get_string (client, "argument", TRUE, &logging_client->argument, error) ||
        !nuvasha_mc_get_string (client, "type", TRUE, &logging_client->type, error) ||
        !get_download_member (client, "file", TRUE, &logging_client->file, error))
      {
        nuvasha_mc_free_logging (logging_client);
        return FALSE;
      }

    *logging = logging_client;
  }

  return TRUE;
}

static gboolean
parse_java_version (JsonObject *root, NuvashaJavaVersion **version, GError **error)
{
  JsonObject *object = NULL;
  NuvashaJavaVersion *java_version;
  gint64 major = 0;

  *version = NULL;

  if (!nuvasha_mc_get_object (root, "javaVersion", FALSE, &object, error))
    return FALSE;

  if (object == NULL)
    return TRUE;

  java_version = g_new0 (NuvashaJavaVersion, 1);

  if (!nuvasha_mc_get_string (object, "component", TRUE, &java_version->component, error) ||
      !nuvasha_mc_get_int64 (object, "majorVersion", TRUE, &major, error))
    {
      nuvasha_mc_free_java_version (java_version);
      return FALSE;
    }

  if (major < 0 || major > G_MAXINT)
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "the Java major version is out of range");
      nuvasha_mc_free_java_version (java_version);
      return FALSE;
    }

  java_version->major_version = (int) major;
  *version = java_version;

  return TRUE;
}

/* ---------------------------------------------------------- *
 * NuvashaVersionDescription
 * ---------------------------------------------------------- */

G_DEFINE_FINAL_TYPE (NuvashaVersionDescription, nuvasha_version_description,
                     G_TYPE_OBJECT)

static gboolean
fill_version (JsonObject *root, NuvashaVersionDescription *self, GError **error)
{
  JsonObject *downloads = NULL;
  g_autofree char *type_name = NULL;

  if (!nuvasha_mc_get_string (root, "id", TRUE, &self->id, error))
    return FALSE;

  if (!nuvasha_mc_get_string (root, "type", TRUE, &type_name, error))
    return FALSE;

  if (!nuvasha_mc_version_type_from_string (type_name, &self->type))
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "unknown version type \"%s\"", type_name);
      return FALSE;
    }

  /* Everything below is optional: a loader profile that inherits from
   * a real version legitimately carries only some of it. */
  if (!nuvasha_mc_get_string (root, "mainClass", FALSE, &self->main_class, error) ||
      !nuvasha_mc_get_string (root, "inheritsFrom", FALSE, &self->inherits_from, error) ||
      !nuvasha_mc_get_string (root, "assets", FALSE, &self->assets, error) ||
      !nuvasha_mc_get_string (root, "minecraftArguments", FALSE,
                   &self->minecraft_arguments, error))
    return FALSE;

  if (!get_download_member (root, "assetIndex", TRUE, &self->asset_index, error))
    return FALSE;

  if (!nuvasha_mc_get_object (root, "downloads", FALSE, &downloads, error))
    return FALSE;

  if (downloads != NULL &&
      !get_download_member (downloads, "client", TRUE, &self->client, error))
    return FALSE;

  if (!parse_libraries (root, &self->libraries, error))
    return FALSE;

  if (!parse_arguments (root, self, error))
    return FALSE;

  if (!parse_logging (root, &self->logging, error))
    return FALSE;

  return parse_java_version (root, &self->java_version, error);
}

NuvashaVersionDescription *
nuvasha_version_description_new_from_json (const char *json,
                                       gssize      length,
                                       GError    **error)
{
  g_autoptr (JsonParser) parser = NULL;
  NuvashaVersionDescription *self;
  JsonObject *root;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (json != NULL, NULL);

  root = nuvasha_mc_root_object (json, length, &parser, error);

  if (root == NULL)
    return NULL;

  self = g_object_new (nuvasha_TYPE_VERSION_DESCRIPTION, NULL);

  if (!fill_version (root, self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

static void
nuvasha_version_description_finalize (GObject *object)
{
  NuvashaVersionDescription *self = nuvasha_VERSION_DESCRIPTION (object);

  g_free (self->id);
  g_free (self->main_class);
  g_free (self->inherits_from);
  g_free (self->assets);
  g_free (self->minecraft_arguments);
  g_clear_pointer (&self->asset_index, nuvasha_mc_free_download_info);
  g_clear_pointer (&self->client, nuvasha_mc_free_download_info);
  g_clear_pointer (&self->logging, nuvasha_mc_free_logging);
  g_clear_pointer (&self->java_version, nuvasha_mc_free_java_version);
  g_clear_pointer (&self->libraries, g_ptr_array_unref);
  g_clear_pointer (&self->jvm_arguments, g_ptr_array_unref);
  g_clear_pointer (&self->game_arguments, g_ptr_array_unref);

  G_OBJECT_CLASS (nuvasha_version_description_parent_class)->finalize (object);
}

static void
nuvasha_version_description_class_init (NuvashaVersionDescriptionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nuvasha_version_description_finalize;
}

static void
nuvasha_version_description_init (NuvashaVersionDescription *self)
{
  (void) self;
}

/* ---------------------------------------------------------- *
 * NuvashaVersionManifest
 * ---------------------------------------------------------- */
