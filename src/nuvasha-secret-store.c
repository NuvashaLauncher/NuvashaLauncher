/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-secret-store.h"

#include <json-glib/json-glib.h>
#include <libsecret/secret.h>

static const SecretSchema schema = {
  "com.github.nuvashalauncher.NuvashaAccount",
  SECRET_SCHEMA_NONE,
  { { "account-id", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } },
  0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static char *
token_payload (const char *access_token, const char *refresh_token)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  JsonNode *root;
  char *payload;

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "access-token");
  json_builder_add_string_value (builder, access_token);
  json_builder_set_member_name (builder, "refresh-token");
  json_builder_add_string_value (builder, refresh_token);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  payload = json_generator_to_data (generator, NULL);
  json_node_free (root);
  return payload;
}

gboolean
nuvasha_secret_store_save (const char *account_id,
                           const char *access_token,
                           const char *refresh_token)
{
  g_autofree char *payload = NULL;
  g_autoptr (GError) error = NULL;

  if (account_id == NULL || *account_id == '\0' ||
      access_token == NULL || refresh_token == NULL)
    return FALSE;

  payload = token_payload (access_token, refresh_token);
  if (!secret_password_store_sync (&schema, SECRET_COLLECTION_DEFAULT,
                                   "Nuvasha Minecraft account", payload,
                                   NULL, &error,
                                   "account-id", account_id, NULL))
    return FALSE;

  return TRUE;
}

gboolean
nuvasha_secret_store_load (const char *account_id,
                           char      **access_token,
                           char      **refresh_token)
{
  g_autofree char *payload = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  JsonObject *object;
  const char *access;
  const char *refresh;

  g_return_val_if_fail (access_token != NULL, FALSE);
  g_return_val_if_fail (refresh_token != NULL, FALSE);
  g_clear_pointer (access_token, g_free);
  g_clear_pointer (refresh_token, g_free);

  if (account_id == NULL || *account_id == '\0')
    return FALSE;
  payload = secret_password_lookup_sync (&schema, NULL, &error,
                                         "account-id", account_id, NULL);
  if (payload == NULL)
    return FALSE;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, payload, -1, &error))
    return FALSE;
  object = json_node_get_object (json_parser_get_root (parser));
  if (object == NULL)
    return FALSE;
  access = json_object_get_string_member_with_default (object, "access-token", NULL);
  refresh = json_object_get_string_member_with_default (object, "refresh-token", NULL);
  if (access == NULL || refresh == NULL)
    return FALSE;

  *access_token = g_strdup (access);
  *refresh_token = g_strdup (refresh);
  return TRUE;
}

gboolean
nuvasha_secret_store_clear (const char *account_id, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (account_id == NULL || *account_id == '\0')
    return FALSE;

  if (secret_password_clear_sync (&schema, NULL, error,
                                  "account-id", account_id, NULL))
    return TRUE;

  /* libsecret reports FALSE without an error when there was no matching
   * item. That is already the desired state for a clear operation. */
  return error == NULL || *error == NULL;
}
