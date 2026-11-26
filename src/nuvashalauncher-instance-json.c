/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* JSON serialization and parsing for NuvashaLauncherInstance. */
#include "nuvashalauncher-instance-private.h"
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
      g_set_error (error, NUVASHALAUNCHER_INSTANCE_ERROR, NUVASHALAUNCHER_INSTANCE_ERROR_MISSING_FIELD,
                   "instance is missing \"%s\"", member);
      return NULL;
    }

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, NUVASHALAUNCHER_INSTANCE_ERROR, NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
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
      g_set_error (error, NUVASHALAUNCHER_INSTANCE_ERROR, NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
                   "instance field \"%s\" must be a string", member);
      return FALSE;
    }

  const char *string = json_node_get_string (node);

  if (string != NULL && string[0] != '\0')
    *value = string;

  return TRUE;
}

static gboolean
get_loader (JsonObject *object, NuvashaLauncherLoader *loader, GError **error)
{
  const char *string = get_required_string (object, "loader", error);

  if (string == NULL)
    return FALSE;

  if (!nuvashalauncher_loader_from_string (string, loader))
    {
      g_set_error (error, NUVASHALAUNCHER_INSTANCE_ERROR, NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
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
      g_set_error (error, NUVASHALAUNCHER_INSTANCE_ERROR, NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
                   "instance field \"jvm_args\" must be a string");
      return FALSE;
    }

  const char *string = json_node_get_string (node);

  if (string == NULL)
    {
      g_set_error (error, NUVASHALAUNCHER_INSTANCE_ERROR, NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
                   "instance field \"jvm_args\" must not be null");
      return FALSE;
    }

  *jvm_args = string;
  return TRUE;
}

static gint64
get_nonneg_int64 (JsonObject *object, const char *member)
{
  if (!json_object_has_member (object, member))
    return 0;

  JsonNode *node = json_object_get_member (object, member);
  if (JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_INT64 &&
      json_node_get_int (node) > 0)
    return json_node_get_int (node);

  return 0;
}

static guint
get_nonneg_uint (JsonObject *object, const char *member)
{
  gint64 val = get_nonneg_int64 (object, member);

  return val > G_MAXUINT ? G_MAXUINT : (guint) val;
}

static gint
get_tristate_bool (JsonObject *object, const char *member)
{
  if (!json_object_has_member (object, member))
    return -1;
  JsonNode *node = json_object_get_member (object, member);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return -1;
  if (json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    return json_node_get_boolean (node) ? 1 : 0;
  if (json_node_get_value_type (node) == G_TYPE_INT64)
    {
      gint64 v = json_node_get_int (node);
      if (v == 0 || v == 1)
        return (gint) v;
    }
  return -1;
}

NuvashaLauncherInstance *
nuvashalauncher_instance_new_from_json (JsonNode *node, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    {
      g_set_error (error, NUVASHALAUNCHER_INSTANCE_ERROR, NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
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

  NuvashaLauncherLoader loader;
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

  guint memory_mib = 0;
  if (json_object_has_member (object, "memory_mib"))
    {
      JsonNode *node = json_object_get_member (object, "memory_mib");
      if (!JSON_NODE_HOLDS_NULL (node))
        {
          if (!JSON_NODE_HOLDS_VALUE (node) ||
              json_node_get_value_type (node) != G_TYPE_INT64)
            {
              g_set_error_literal (error, NUVASHALAUNCHER_INSTANCE_ERROR,
                                   NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
                                   "instance field \"memory_mib\" must be an integer");
              return NULL;
            }
          gint64 val = json_node_get_int (node);
          if (val < 0 || val > 1048576)
            {
              g_set_error_literal (error, NUVASHALAUNCHER_INSTANCE_ERROR,
                                   NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,
                                   "instance field \"memory_mib\" is out of range");
              return NULL;
            }
          memory_mib = (guint) val;
        }
    }

  guint game_width = get_nonneg_uint (object, "game_width");
  if (game_width > 7680)
    game_width = 0;
  guint game_height = get_nonneg_uint (object, "game_height");
  if (game_height > 4320)
    game_height = 0;
  gint game_fullscreen = get_tristate_bool (object, "game_fullscreen");
  gint use_dedicated_gpu = get_tristate_bool (object, "use_dedicated_gpu");
  gint use_gamemode = get_tristate_bool (object, "use_gamemode");
  gint use_mangohud = get_tristate_bool (object, "use_mangohud");

  guint64 play_seconds = (guint64) get_nonneg_int64 (object, "play_seconds");
  gint64 last_played_at = get_nonneg_int64 (object, "last_played_at");
  gint64 created_at = get_nonneg_int64 (object, "created_at");
  guint launch_count = get_nonneg_uint (object, "launch_count");
  guint crash_count = get_nonneg_uint (object, "crash_count");

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

  NuvashaLauncherInstance *self =
    nuvashalauncher_instance_new (id, name, directory, minecraft_version, loader, error);

  if (self == NULL)
    return NULL;

  g_object_set (self,
                "loader-version", loader_version,
                "icon", icon,
                 "java-path", java_path,
                 "jvm_args", jvm_args,
                 "memory-mib", memory_mib,
                 "game-width", game_width,
                 "game-height", game_height,
                 "game-fullscreen", game_fullscreen,
                 "use-dedicated-gpu", use_dedicated_gpu,
                 "use-gamemode", use_gamemode,
                 "use-mangohud", use_mangohud,
                 "play-seconds", play_seconds,
                 "last-played-at", last_played_at,
                 "created-at", created_at,
                 "launch-count", launch_count,
                 "crash-count", crash_count,
                 "modpack_name", modpack_name,
                 "modpack_summary", modpack_summary,
                 "modpack_version_id", modpack_version_id,
                 NULL);

  if (!nuvashalauncher_instance_validate (self, error))
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

static void
add_positive_int64 (JsonBuilder *builder,
                    const char  *member_name,
                    gint64       value)
{
  if (value <= 0)
    return;

  json_builder_set_member_name (builder, member_name);
  json_builder_add_int_value (builder, value);
}

JsonNode *
nuvashalauncher_instance_to_json (NuvashaLauncherInstance *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_INSTANCE (self), NULL);

  if (!nuvashalauncher_instance_validate (self, NULL))
    {
      g_critical ("NuvashaLauncherInstance: refusing to serialize an invalid instance");
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
                                 nuvashalauncher_loader_to_string (self->loader));

  add_optional_member (builder, "loader_version", self->loader_version);
  add_optional_member (builder, "icon", self->icon);
  add_optional_member (builder, "java_path", self->java_path);

  add_optional_member (builder, "jvm_args", self->jvm_args);
  add_positive_int64 (builder, "memory_mib", (gint64) self->memory_mib);
  add_positive_int64 (builder, "game_width", (gint64) self->game_width);
  add_positive_int64 (builder, "game_height", (gint64) self->game_height);
  if (self->game_fullscreen >= 0)
    {
      json_builder_set_member_name (builder, "game_fullscreen");
      json_builder_add_boolean_value (builder, self->game_fullscreen != 0);
    }
  if (self->use_dedicated_gpu >= 0)
    {
      json_builder_set_member_name (builder, "use_dedicated_gpu");
      json_builder_add_boolean_value (builder, self->use_dedicated_gpu != 0);
    }
  if (self->use_gamemode >= 0)
    {
      json_builder_set_member_name (builder, "use_gamemode");
      json_builder_add_boolean_value (builder, self->use_gamemode != 0);
    }
  if (self->use_mangohud >= 0)
    {
      json_builder_set_member_name (builder, "use_mangohud");
      json_builder_add_boolean_value (builder, self->use_mangohud != 0);
    }
  add_positive_int64 (builder, "play_seconds", (gint64) self->play_seconds);
  add_positive_int64 (builder, "last_played_at", self->last_played_at);
  add_positive_int64 (builder, "created_at", self->created_at);
  add_positive_int64 (builder, "launch_count", (gint64) self->launch_count);
  add_positive_int64 (builder, "crash_count", (gint64) self->crash_count);
  add_optional_member (builder, "modpack_name", self->modpack_name);
  add_optional_member (builder, "modpack_summary", self->modpack_summary);
  add_optional_member (builder, "modpack_version_id", self->modpack_version_id);

  json_builder_end_object (builder);

  return json_builder_get_root (builder);
}
