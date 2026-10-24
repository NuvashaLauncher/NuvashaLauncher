/* JSON serialization and parsing for NuvashaInstance. */
#include "nuvasha-instance-private.h"
#include <string.h>

#define DEFAULT_JVM_ARGS ""

static const char *
get_required_string (JsonObject  *object,
                     const char  *member,
                     GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_MISSING_FIELD,
                   "instance is missing \"%s\"", member);
      return NULL;
    }

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance field \"%s\" must be a string", member);
      return NULL;
    }

  return json_node_get_string (node);
}

/* Reads an optional string member: absent, null and empty all become
 * NULL, which is how the nullable fields are stored. */
static gboolean
get_optional_string (JsonObject   *object,
                     const char   *member,
                     const char  **value,
                     GError      **error)
{
  *value = NULL;

  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance field \"%s\" must be a string", member);
      return FALSE;
    }

  const char *string = json_node_get_string (node);

  if (string != NULL && string[0] != '\0')
    *value = string;

  return TRUE;
}

static gboolean
get_loader (JsonObject *object, NuvashaLoader *loader, GError **error)
{
  const char *string = get_required_string (object, "loader", error);

  if (string == NULL)
    return FALSE;

  if (!nuvasha_loader_from_string (string, loader))
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "unknown mod loader \"%s\"", string);
      return FALSE;
    }

  return TRUE;
}

/* "jvm_args" is optional and may be an empty string: an instance the
   * user never gave any arguments to has none, and the launcher adds
   * nothing from it. */
static gboolean
get_jvm_args (JsonObject *object, const char **jvm_args, GError **error)
{
  JsonNode *node = json_object_get_member (object, "jvm_args");

  if (node == NULL)
    {
      *jvm_args = DEFAULT_JVM_ARGS;
      return TRUE;
    }

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance field \"jvm_args\" must be a string");
      return FALSE;
    }

  const char *string = json_node_get_string (node);

  if (string == NULL)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance field \"jvm_args\" must not be null");
      return FALSE;
    }

  *jvm_args = string;
  return TRUE;
}

NuvashaInstance *
nuvasha_instance_new_from_json (JsonNode *node, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance entry is not a JSON object");
      return NULL;
    }

  JsonObject *object = json_node_get_object (node);

  const char *id = get_required_string (object, "id", error);
  if (id == NULL)
    return NULL;

  const char *name = get_required_string (object, "name", error);
  if (name == NULL)
    return NULL;

  const char *directory = get_required_string (object, "directory", error);
  if (directory == NULL)
    return NULL;

  const char *minecraft_version =
    get_required_string (object, "minecraft_version", error);
  if (minecraft_version == NULL)
    return NULL;

  NuvashaLoader loader;
  if (!get_loader (object, &loader, error))
    return NULL;

  const char *loader_version = NULL;
  if (!get_optional_string (object, "loader_version", &loader_version, error))
    return NULL;

  const char *icon = NULL;
  if (!get_optional_string (object, "icon", &icon, error))
    return NULL;

  const char *java_path = NULL;
  if (!get_optional_string (object, "java_path", &java_path, error))
    return NULL;

  const char *jvm_args = NULL;
  if (!get_jvm_args (object, &jvm_args, error))
    return NULL;

  const char *modpack_name = NULL;
  if (!get_optional_string (object, "modpack_name", &modpack_name, error))
    return NULL;

  const char *modpack_summary = NULL;
  if (!get_optional_string (object, "modpack_summary", &modpack_summary, error))
    return NULL;

  const char *modpack_version_id = NULL;
  if (!get_optional_string (object, "modpack_version_id", &modpack_version_id,
                            error))
    return NULL;

  NuvashaInstance *self =
    nuvasha_instance_new (id, name, directory, minecraft_version, loader, error);

  if (self == NULL)
    return NULL;

  g_object_set (self,
                "loader_version", loader_version,
                "icon", icon,
                 "java_path", java_path,
                 "jvm_args", jvm_args,
                 "modpack_name", modpack_name,
                 "modpack_summary", modpack_summary,
                 "modpack_version_id", modpack_version_id,
                 NULL);

  if (!nuvasha_instance_validate (self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

static void
add_optional_member (JsonBuilder *builder,
                     const char  *member_name,
                     const char  *value)
{
  json_builder_set_member_name (builder, member_name);

  if (value == NULL)
    json_builder_add_null_value (builder);
  else
    json_builder_add_string_value (builder, value);
}

JsonNode *
nuvasha_instance_to_json (NuvashaInstance *self)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), NULL);

  if (!nuvasha_instance_validate (self, NULL))
    {
      g_critical ("NuvashaInstance: refusing to serialize an invalid instance");
      return NULL;
    }

  g_autoptr (JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "id");
  json_builder_add_string_value (builder, self->id);

  json_builder_set_member_name (builder, "name");
  json_builder_add_string_value (builder, self->name);

  json_builder_set_member_name (builder, "directory");
  json_builder_add_string_value (builder, self->directory);

  json_builder_set_member_name (builder, "minecraft_version");
  json_builder_add_string_value (builder, self->minecraft_version);

  json_builder_set_member_name (builder, "loader");
  json_builder_add_string_value (builder,
                                 nuvasha_loader_to_string (self->loader));

  add_optional_member (builder, "loader_version", self->loader_version);
  add_optional_member (builder, "icon", self->icon);
  add_optional_member (builder, "java_path", self->java_path);

  add_optional_member (builder, "jvm_args", self->jvm_args);
  add_optional_member (builder, "modpack_name", self->modpack_name);
  add_optional_member (builder, "modpack_summary", self->modpack_summary);
  add_optional_member (builder, "modpack_version_id", self->modpack_version_id);

  json_builder_end_object (builder);

  return json_builder_get_root (builder);
}
