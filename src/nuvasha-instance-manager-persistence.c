/* Loading and saving instances.json. */
#include "nuvasha-instance-manager-private.h"
#include "nuvasha-i18n.h"
#include "nuvasha-fs.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define INSTANCES_FORMAT_VERSION 1
#define METADATA_FILE "instances.json"
#define CORRUPT_SUFFIX ".corrupt"

static char *
build_metadata_json (NuvashaInstanceManager *self, GError **error)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "version");
  json_builder_add_int_value (builder, INSTANCES_FORMAT_VERSION);

  json_builder_set_member_name (builder, "instances");
  json_builder_begin_array (builder);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (NuvashaInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);

      if (!nuvasha_instance_validate (instance, error))
        return NULL;

      /* The builder takes ownership of the node. */
      json_builder_add_value (builder, nuvasha_instance_to_json (instance));
    }

  json_builder_end_array (builder);
  json_builder_end_object (builder);

  g_autoptr (JsonNode) root = json_builder_get_root (builder);
  g_autoptr (JsonGenerator) generator = json_generator_new ();

  json_generator_set_pretty (generator, TRUE);
  json_generator_set_root (generator, root);

  return json_generator_to_data (generator, NULL);
}

gboolean
nuvasha_instance_manager_save (NuvashaInstanceManager *self, GError **error)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_autofree char *json = build_metadata_json (self, error);

  if (json == NULL)
    return FALSE;

  g_autoptr (GError) directory_error = NULL;
  if (!nuvasha_fs_make_directory (self->data_dir, 0700, &directory_error))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_IO,
                   "cannot create \"%s\": %s",
                    self->data_dir, directory_error->message);
      return FALSE;
    }

  g_autoptr (GError) local_error = NULL;

  /* The data goes to a temporary file in the same directory, is flushed
   * and fsynced and then renamed over instances.json, so a save that
   * fails or is interrupted can never leave a half written file. */
  if (!nuvasha_fs_replace_contents (self->metadata_path, json, -1, 0600,
                                &local_error))
    {
      g_prefix_error (&local_error, "cannot save \"%s\": ",
                      self->metadata_path);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

static gboolean
get_metadata_version (JsonObject *object, GError **error)
{
  JsonNode *node = json_object_get_member (object, "version");

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_INT64)
    {
      if (json_node_get_int (node) == INSTANCES_FORMAT_VERSION)
        return TRUE;

      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "unsupported instances.json version %" G_GINT64_FORMAT,
                   json_node_get_int (node));
      return FALSE;
    }

  g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
               nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT,
               "instances.json has no integer \"version\"");
  return FALSE;
}

/* Parses the whole file into a list of instances. Any problem fails
 * with a GError and returns NULL: a file is either fully understood or
 * treated as corrupt. */
static GList *
parse_instances_json (const char *contents, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, contents, length, error))
    return NULL;

  JsonNode *root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "instances.json is not a JSON object");
      return NULL;
    }

  JsonObject *object = json_node_get_object (root);

  if (!get_metadata_version (object, error))
    return NULL;

  JsonNode *array_node = json_object_get_member (object, "instances");

  if (array_node == NULL || !JSON_NODE_HOLDS_ARRAY (array_node))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "instances.json has no \"instances\" array");
      return NULL;
    }

  JsonArray *array = json_node_get_array (array_node);
  guint n_items = json_array_get_length (array);
  GList *list = NULL;

  for (guint i = 0; i < n_items; i++)
    {
      NuvashaInstance *instance =
        nuvasha_instance_new_from_json (json_array_get_element (array, i), error);

      if (instance == NULL)
        {
          g_list_free_full (list, g_object_unref);
          return NULL;
        }

      const char *id = nuvasha_instance_get_id (instance);
      g_autofree char *instance_name_value = instance_name (instance);
      g_autofree char *instance_key =
        instance_name_key (instance_name_value);

      for (GList *l = list; l != NULL; l = l->next)
        {
          NuvashaInstance *seen = l->data;
          g_autofree char *seen_name = instance_name (seen);
          g_autofree char *seen_key =
            instance_name_key (seen_name);

          if (g_str_equal (nuvasha_instance_get_id (seen), id))
            {
              g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                           nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT,
                           "instances.json lists the id \"%s\" twice", id);
              g_object_unref (instance);
              g_list_free_full (list, g_object_unref);
              return NULL;
            }

          if (g_str_equal (seen_key, instance_key))
            {
              g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                           nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT,
                           "instances.json lists the normalized name \"%s\" "
                           "more than once",
                           instance_name_value);
              g_object_unref (instance);
              g_list_free_full (list, g_object_unref);
              return NULL;
            }
        }

      list = g_list_prepend (list, instance);
    }

  return g_list_reverse (list);
}

/* Keeps an unreadable file around instead of deleting it. */
static void
quarantine_corrupt_file (const char *path)
{
  g_autofree char *base = g_strconcat (path, CORRUPT_SUFFIX, NULL);
  g_autofree char *corrupt_path = NULL;

  for (guint i = 0; i < 1000; i++)
    {
      corrupt_path = i == 0 ? g_strdup (base)
                            : g_strdup_printf ("%s.%u", base, i);
      if (!g_file_test (corrupt_path, G_FILE_TEST_EXISTS))
        break;
      g_clear_pointer (&corrupt_path, g_free);
    }

  if (corrupt_path == NULL || !nuvasha_fs_rename (path, corrupt_path, NULL))
    g_warning ("could not rename the corrupt file to \"%s\": %s",
               corrupt_path, g_strerror (errno));
}

gboolean
nuvasha_instance_manager_load (NuvashaInstanceManager *self, GError **error)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_autofree char *contents = NULL;
  gsize length = 0;
  g_autoptr (GError) local_error = NULL;

  if (!nuvasha_fs_get_contents_within (self->metadata_path,
                                   nuvasha_FS_MAX_INSTANCE_METADATA_SIZE,
                                   &contents, &length, &local_error))
    {
      /* First run: there simply is nothing to load yet. */
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_list_store_remove_all (self->instances);
          return TRUE;
        }

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  GList *list = parse_instances_json (contents, length, &local_error);

  /* A failure is signalled by the error, not by the list: a file with
   * an empty "instances" array parses to a NULL list just fine. */
  if (local_error != NULL)
    {
      /* Never delete what we do not understand: keep it as
       * instances.json.corrupt and start empty. */
      quarantine_corrupt_file (self->metadata_path);
      g_list_store_remove_all (self->instances);

      g_prefix_error (&local_error, "instances.json is unusable: ");
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_list_store_remove_all (self->instances);

  for (GList *l = list; l != NULL; l = l->next)
    g_list_store_append (self->instances, l->data);

  g_list_free_full (list, g_object_unref);

  /* A directory removed while nuvasha was not running means the instance was
   * removed outside nuvasha. Reflect that in the model and metadata as well. */
  check_instance_directories (self);

  return TRUE;
}
