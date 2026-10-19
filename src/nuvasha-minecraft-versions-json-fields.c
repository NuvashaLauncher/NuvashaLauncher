/* Shared JSON field readers for Minecraft metadata. */

#include "nuvasha-minecraft-versions-json-private.h"

#include <math.h>

gboolean
nuvasha_mc_missing_field (const char *member, GError **error)
{
  g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_MISSING,
               "required field \"%s\" is missing", member);
  return FALSE;
}

gboolean
nuvasha_mc_wrong_field (const char *member, const char *expected, GError **error)
{
  g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
               "field \"%s\" must be %s", member, expected);
  return FALSE;
}

/* *value receives an owned copy, or NULL when the member is absent.
 * @required decides whether "absent" is an error at all. */
gboolean
nuvasha_mc_get_string (JsonObject  *object,
            const char  *member,
            gboolean     required,
            char       **value,
            GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || nuvasha_mc_missing_field (member, error);

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return nuvasha_mc_wrong_field (member, "a string", error);

  *value = g_strdup (json_node_get_string (node));

  if (required && (*value)[0] == '\0')
    {
      g_clear_pointer (value, g_free);
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_MISSING,
                   "required field \"%s\" is empty", member);
      return FALSE;
    }

  return TRUE;
}

/* The object itself is borrowed from the parser, so the caller only
 * uses it while the document is alive. */
gboolean
nuvasha_mc_get_object (JsonObject   *object,
            const char   *member,
            gboolean      required,
            JsonObject  **value,
            GError      **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || nuvasha_mc_missing_field (member, error);

  if (!JSON_NODE_HOLDS_OBJECT (node))
    return nuvasha_mc_wrong_field (member, "an object", error);

  *value = json_node_get_object (node);
  return TRUE;
}

gboolean
nuvasha_mc_get_array (JsonObject  *object,
           const char  *member,
           gboolean     required,
           JsonArray  **value,
           GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || nuvasha_mc_missing_field (member, error);

  if (!JSON_NODE_HOLDS_ARRAY (node))
    return nuvasha_mc_wrong_field (member, "an array", error);

  *value = json_node_get_array (node);
  return TRUE;
}

gboolean
nuvasha_mc_get_int64 (JsonObject *object,
           const char *member,
           gboolean    required,
           gint64     *value,
           GError    **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = 0;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || nuvasha_mc_missing_field (member, error);

  if (JSON_NODE_HOLDS_VALUE (node))
    {
      GType type = json_node_get_value_type (node);

      if (type == G_TYPE_INT64)
        {
          *value = json_node_get_int (node);
          return TRUE;
        }

      if (type == G_TYPE_DOUBLE)
        {
          gdouble number = json_node_get_double (node);

          if (!isfinite (number) || number < -9223372036854775808.0 ||
              number >= 9223372036854775808.0)
            return nuvasha_mc_wrong_field (member, "an integer in range", error);

          *value = (gint64) number;
          return TRUE;
        }
    }

  return nuvasha_mc_wrong_field (member, "a number", error);
}

/* Parses @json and returns the root object, handing back the parser
 * it came from so that borrowed nodes stay valid. The caller unrefs
 * it. */
JsonObject *
nuvasha_mc_root_object (const char *json, gssize length, JsonParser **parser, GError **error)
{
  JsonParser *document = json_parser_new ();
  g_autoptr (GError) json_error = NULL;
  JsonNode *root;

  if (!json_parser_load_from_data (document, json, length, &json_error))
    {
      /* The domain changes so that a caller only has to know
       * nuvasha_MC_ERROR for a document that will not read. */
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "the document is not JSON: %s", json_error->message);
      g_object_unref (document);
      return NULL;
    }

  root = json_parser_get_root (document);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "the document is not a JSON object");
      g_object_unref (document);
      return NULL;
    }

  *parser = document;
  return json_node_get_object (root);
}
