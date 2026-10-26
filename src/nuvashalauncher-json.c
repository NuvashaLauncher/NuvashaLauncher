/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-json.h"

#include <math.h>

GQuark
nuvashalauncher_json_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-json-error");
}

JsonObject *
nuvashalauncher_json_object_of (JsonNode *node)
{
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return NULL;

  return json_node_get_object (node);
}

char *
nuvashalauncher_json_string (JsonNode *node, const char *name)
{
  JsonObject *object = nuvashalauncher_json_object_of (node);
  JsonNode *member = object != NULL ? json_object_get_member (object, name)
                                    : NULL;

  if (member == NULL || !JSON_NODE_HOLDS_VALUE (member) ||
      json_node_get_value_type (member) != G_TYPE_STRING)
    return NULL;

  return json_node_dup_string (member);
}

gboolean
nuvashalauncher_json_boolean (JsonNode *node, const char *name, gboolean fallback)
{
  JsonObject *object = nuvashalauncher_json_object_of (node);
  JsonNode *member = object != NULL ? json_object_get_member (object, name)
                                    : NULL;

  if (member == NULL || !JSON_NODE_HOLDS_VALUE (member) ||
      json_node_get_value_type (member) != G_TYPE_BOOLEAN)
    return fallback;

  return json_node_get_boolean (member);
}

gint64
nuvashalauncher_json_int (JsonNode *node, const char *name, gint64 fallback)
{
  JsonObject *object = nuvashalauncher_json_object_of (node);
  JsonNode *member = object != NULL ? json_object_get_member (object, name)
                                    : NULL;
  GType type;

  if (member == NULL || !JSON_NODE_HOLDS_VALUE (member))
    return fallback;

  type = json_node_get_value_type (member);
  if (type == G_TYPE_INT64)
    return json_node_get_int (member);

  if (type == G_TYPE_DOUBLE)
    {
      gdouble number = json_node_get_double (member);

      if (!isfinite (number) || number < -9223372036854775808.0 ||
          number >= 9223372036854775808.0)
        return fallback;

      return (gint64) number;
    }

  return fallback;
}

GPtrArray *
nuvashalauncher_json_string_array (JsonNode *node, const char *name)
{
  GPtrArray *strings = g_ptr_array_new_with_free_func (g_free);
  JsonObject *object = nuvashalauncher_json_object_of (node);
  JsonNode *member = object != NULL ? json_object_get_member (object, name)
                                    : NULL;

  if (member == NULL || !JSON_NODE_HOLDS_ARRAY (member))
    return strings;

  JsonArray *array = json_node_get_array (member);
  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *item = json_array_get_element (array, i);

      if (JSON_NODE_HOLDS_VALUE (item) &&
          json_node_get_value_type (item) == G_TYPE_STRING)
        g_ptr_array_add (strings, json_node_dup_string (item));
    }

  return strings;
}

gboolean
nuvashalauncher_json_require_string (JsonNode *node, const char *name,
                         char **value, GError **error)
{
  JsonObject *object = nuvashalauncher_json_object_of (node);
  JsonNode *member = object != NULL ? json_object_get_member (object, name)
                                    : NULL;

  g_return_val_if_fail (value != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (member == NULL)
    {
      g_set_error (error, nuvashalauncher_JSON_ERROR, nuvashalauncher_JSON_ERROR_MISSING,
                   "required JSON field \"%s\" is missing", name);
      return FALSE;
    }

  if (!JSON_NODE_HOLDS_VALUE (member) ||
      json_node_get_value_type (member) != G_TYPE_STRING ||
      json_node_get_string (member) == NULL ||
      *json_node_get_string (member) == '\0')
    {
      g_set_error (error, nuvashalauncher_JSON_ERROR, nuvashalauncher_JSON_ERROR_TYPE,
                   "JSON field \"%s\" is not a non-empty string", name);
      return FALSE;
    }

  *value = json_node_dup_string (member);
  return TRUE;
}
