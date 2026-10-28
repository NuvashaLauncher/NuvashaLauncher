/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-secret-store.h"
#include "nuvashalauncher-accounts-private.h"

#include <json-glib/json-glib.h>
#include <libsecret/secret.h>

/* This is deliberately a new schema. Older keyring entries are not part of
 * the current account store and are neither read nor migrated. */
static const SecretSchema account_schema = {
  "com.github.nuvashalauncher.Account",
  SECRET_SCHEMA_NONE,
  { { "account-id", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } },
  0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static const SecretSchema active_schema = {
  "com.github.nuvashalauncher.ActiveAccount",
  SECRET_SCHEMA_NONE,
  { { "kind", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } },
  0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static gboolean
secret_error (GError **error, GError *cause)
{
  if (cause != NULL)
    g_propagate_error (error, cause);
  else
    g_set_error (error, nuvashalauncher_ACCOUNTS_ERROR,
                 nuvashalauncher_ACCOUNTS_ERROR_IO,
                 "Secret Service returned no account data");
  return FALSE;
}

static char *
account_payload (const nuvashalauncherAccount *account)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  char *payload = NULL;

  account_to_json (builder, account);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  payload = json_generator_to_data (generator, NULL);
  return payload;
}

static nuvashalauncherAccount *
account_from_payload (const char *payload)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root = NULL;

  if (payload == NULL || !json_parser_load_from_data (parser, payload, -1, NULL))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;

  return account_from_json (json_node_get_object (root));
}

gboolean
nuvashalauncher_secret_store_load (GPtrArray **accounts,
                                   char      **active_id,
                                   GError    **error)
{
  g_autoptr (GError) local_error = NULL;
  g_autofree char *active = NULL;
  g_autoptr (SecretService) service = NULL;
  g_autoptr (GHashTable) attributes = NULL;
  GList *items = NULL;
  GPtrArray *loaded = NULL;

  g_return_val_if_fail (accounts != NULL, FALSE);
  g_return_val_if_fail (active_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  service = secret_service_get_sync (SECRET_SERVICE_NONE, NULL, &local_error);
  if (service == NULL)
    return secret_error (error, g_steal_pointer (&local_error));

  loaded = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_account_free);
  attributes = g_hash_table_new (g_str_hash, g_str_equal);
  items = secret_service_search_sync (service, &account_schema, attributes,
                                      SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                      NULL, &local_error);
  if (local_error != NULL)
    {
      g_ptr_array_unref (loaded);
      return secret_error (error, g_steal_pointer (&local_error));
    }

  for (GList *item = items; item != NULL; item = item->next)
    {
      g_autoptr (SecretValue) value =
        secret_retrievable_retrieve_secret_sync (SECRET_RETRIEVABLE (item->data),
                                                 NULL, &local_error);
      g_autoptr (nuvashalauncherAccount) account =
        value != NULL ? account_from_payload (secret_value_get_text (value)) : NULL;

      if (local_error != NULL)
        {
          g_list_free_full (items, g_object_unref);
          g_ptr_array_unref (loaded);
          return secret_error (error, g_steal_pointer (&local_error));
        }

      if (account != NULL)
        g_ptr_array_add (loaded, g_steal_pointer (&account));
    }
  g_list_free_full (items, g_object_unref);

  active = secret_password_lookup_sync (&active_schema, NULL, &local_error,
                                        "kind", "active-account", NULL);
  if (local_error != NULL)
    {
      g_ptr_array_unref (loaded);
      return secret_error (error, g_steal_pointer (&local_error));
    }

  *accounts = loaded;
  *active_id = g_steal_pointer (&active);
  return TRUE;
}

gboolean
nuvashalauncher_secret_store_save_account (const nuvashalauncherAccount *account,
                                           GError **error)
{
  g_autofree char *payload = NULL;

  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  payload = account_payload (account);
  if (!secret_password_store_sync (&account_schema, SECRET_COLLECTION_DEFAULT,
                                   "Nuvasha Launcher account", payload, NULL,
                                   error, "account-id", account->id, NULL))
    return FALSE;

  return TRUE;
}

gboolean
nuvashalauncher_secret_store_save_active (const char *account_id, GError **error)
{
  g_return_val_if_fail (account_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!secret_password_store_sync (&active_schema, SECRET_COLLECTION_DEFAULT,
                                   "Nuvasha Launcher active account", account_id,
                                   NULL, error, "kind", "active-account", NULL))
    return FALSE;
  return TRUE;
}

gboolean
nuvashalauncher_secret_store_remove_account (const char *account_id, GError **error)
{
  g_return_val_if_fail (account_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!secret_password_clear_sync (&account_schema, NULL, error,
                                   "account-id", account_id, NULL) &&
      (error == NULL || *error != NULL))
    return FALSE;
  return TRUE;
}

gboolean
nuvashalauncher_secret_store_clear_active (GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!secret_password_clear_sync (&active_schema, NULL, error,
                                   "kind", "active-account", NULL) &&
      (error == NULL || *error != NULL))
    return FALSE;
  return TRUE;
}
