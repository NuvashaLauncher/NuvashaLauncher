/* Account JSON serialization and parsing. */

#include "nuvashalauncher-accounts-private.h"

void
account_to_json (JsonBuilder *builder, const nuvashalauncherAccount *account)
{
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, account->type == nuvashalauncher_ACCOUNT_MICROSOFT ? "microsoft" : "offline");
  json_builder_set_member_name (builder, "id");
  json_builder_add_string_value (builder, account->id);
  json_builder_set_member_name (builder, "name");
  json_builder_add_string_value (builder, account->name);
  json_builder_set_member_name (builder, "uuid");
  json_builder_add_string_value (builder, account->uuid);

  if (account->type == nuvashalauncher_ACCOUNT_MICROSOFT)
    {
      json_builder_set_member_name (builder, "access-token");
      if (account->access_token != NULL)
        json_builder_add_string_value (builder, account->access_token);
      else
        json_builder_add_null_value (builder);
      json_builder_set_member_name (builder, "refresh-token");
      if (account->refresh_token != NULL)
        json_builder_add_string_value (builder, account->refresh_token);
      else
        json_builder_add_null_value (builder);
      json_builder_set_member_name (builder, "expires-at");
      json_builder_add_int_value (builder, account->expires_at);
    }

  json_builder_set_member_name (builder, "avatar");
  account->avatar_url != NULL ? json_builder_add_string_value (builder, account->avatar_url)
                              : json_builder_add_null_value (builder);
  json_builder_set_member_name (builder, "skin-model");
  account->skin_model != NULL ? json_builder_add_string_value (builder, account->skin_model)
                              : json_builder_add_null_value (builder);
  json_builder_set_member_name (builder, "cape-id");
  account->cape_id != NULL ? json_builder_add_string_value (builder, account->cape_id)
                           : json_builder_add_null_value (builder);
  json_builder_end_object (builder);
}

char *
string_member (JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member (object, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  return g_strdup (json_node_get_string (node));
}

nuvashalauncherAccount *
account_from_json (JsonObject *object)
{
  g_autofree char *type = string_member (object, "type");
  g_autofree char *id = string_member (object, "id");
  g_autofree char *name = string_member (object, "name");
  g_autofree char *uuid = string_member (object, "uuid");
  nuvashalauncherAccount *account;

  if (type == NULL || id == NULL || *id == '\0' || name == NULL ||
      *name == '\0' || uuid == NULL || *uuid == '\0')
    return NULL;
  if (!g_str_equal (type, "microsoft") && !g_str_equal (type, "offline"))
    return NULL;

  account = g_new0 (nuvashalauncherAccount, 1);
  account->type = g_str_equal (type, "microsoft") ? nuvashalauncher_ACCOUNT_MICROSOFT
                                                  : nuvashalauncher_ACCOUNT_OFFLINE;
  account->id = g_steal_pointer (&id);
  account->name = g_steal_pointer (&name);
  account->uuid = g_steal_pointer (&uuid);
  if (account->type == nuvashalauncher_ACCOUNT_MICROSOFT)
    {
      account->access_token = string_member (object, "access-token");
      account->refresh_token = string_member (object, "refresh-token");
    }
  account->avatar_url = string_member (object, "avatar");
  account->skin_model = string_member (object, "skin-model");
  account->cape_id = string_member (object, "cape-id");
  {
    JsonNode *node = json_object_get_member (object, "expires-at");
    if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
        json_node_get_value_type (node) == G_TYPE_INT64)
      account->expires_at = json_node_get_int (node);
  }
  return account;
}
