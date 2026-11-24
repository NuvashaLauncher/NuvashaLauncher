/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-secret-store.h"
#include "nuvashalauncher-accounts-private.h"

#include <json-glib/json-glib.h>
#include <libsecret/secret.h>

/* This is deliberately a new schema. Older keyring entries are not part of
 * the current account store and are neither read nor migrated. */
static const SecretSchema account_schema = {
  .name = "com.github.NuvashaLauncher.Account",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "account-id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

static const SecretSchema active_schema = {
  .name = "com.github.NuvashaLauncher.ActiveAccount",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "kind", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

/* Tests run against a separate schema so they can never enumerate, update,
 * or remove a real user's account items, even when the test process has
 * access to the user's Secret Service. */
static const SecretSchema test_account_schema = {
  .name = "com.github.NuvashaLauncher.TestAccount",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "account-id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

static const SecretSchema test_active_schema = {
  .name = "com.github.NuvashaLauncher.TestActiveAccount",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "kind", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

static const SecretSchema *
account_schema_for_store (void)
{
  return g_strcmp0 (g_getenv ("NUVASHALAUNCHER_TEST_SECRET_STORE"), "1") == 0
    ? &test_account_schema : &account_schema;
}

static const SecretSchema *
active_schema_for_store (void)
{
  return g_strcmp0 (g_getenv ("NUVASHALAUNCHER_TEST_SECRET_STORE"), "1") == 0
    ? &test_active_schema : &active_schema;
}

static gboolean
secret_error (GError **error, GError *cause)
{
  if (cause != NULL)
    {
      /* Keep the diagnostic, but identify keyring failures as account errors
       * rather than mistaking D-Bus failures for network or file errors. */
      g_set_error_literal (error, NUVASHALAUNCHER_ACCOUNTS_ERROR,
                            NUVASHALAUNCHER_ACCOUNTS_ERROR_IO, cause->message);
      g_error_free (cause);
    }
  else
    g_set_error (error, NUVASHALAUNCHER_ACCOUNTS_ERROR,
                 NUVASHALAUNCHER_ACCOUNTS_ERROR_IO,
                 "Secret Service returned no account data");
  return FALSE;
}

static char *
account_payload (const NuvashaLauncherAccount *account)
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

static NuvashaLauncherAccount *
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

static const char *
test_secret_store_file (void)
{
  const char *path = NULL;

  if (g_strcmp0 (g_getenv ("NUVASHALAUNCHER_TEST_SECRET_STORE"), "1") != 0)
    return NULL;

  path = g_getenv ("NUVASHALAUNCHER_TEST_SECRET_STORE_FILE");
  return (path != NULL && path[0] != '\0') ? path : NULL;
}

static gboolean
file_store_read (const char  *path,
                 GPtrArray  **accounts,
                 char       **active_id,
                 GError     **error)
{
  g_autoptr (GPtrArray) loaded =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_account_free);
  g_autofree char *active = NULL;
  g_autofree char *contents = NULL;
  gsize len = 0;
  g_autoptr (GError) local_error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  JsonNode *root = NULL;
  JsonObject *obj = NULL;

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      *accounts = g_steal_pointer (&loaded);
      *active_id = NULL;
      return TRUE;
    }

  if (!g_file_get_contents (path, &contents, &len, &local_error))
    return secret_error (error, g_steal_pointer (&local_error));

  if (len == 0)
    {
      *accounts = g_steal_pointer (&loaded);
      *active_id = NULL;
      return TRUE;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, (gssize) len, &local_error))
    return secret_error (error, g_steal_pointer (&local_error));

  root = json_parser_get_root (parser);
  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
    {
      obj = json_node_get_object (root);
      if (json_object_has_member (obj, "active") &&
          !json_object_get_null_member (obj, "active"))
        active = g_strdup (json_object_get_string_member (obj, "active"));

      if (json_object_has_member (obj, "accounts"))
        {
          JsonArray *arr = json_object_get_array_member (obj, "accounts");
          guint n = arr != NULL ? json_array_get_length (arr) : 0;

          for (guint i = 0; i < n; i++)
            {
              JsonObject *item = json_array_get_object_element (arr, i);
              g_autoptr (NuvashaLauncherAccount) acc =
                item != NULL ? account_from_json (item) : NULL;

              if (acc != NULL)
                g_ptr_array_add (loaded, g_steal_pointer (&acc));
            }
        }
    }

  *accounts = g_steal_pointer (&loaded);
  *active_id = g_steal_pointer (&active);
  return TRUE;
}

static gboolean
file_store_write (const char *path,
                  GPtrArray  *accounts,
                  const char *active_id,
                  GError    **error)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *data = NULL;
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (GError) local_error = NULL;

  g_mkdir_with_parents (dir, 0700);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "active");
  if (active_id != NULL)
    json_builder_add_string_value (builder, active_id);
  else
    json_builder_add_null_value (builder);

  json_builder_set_member_name (builder, "accounts");
  json_builder_begin_array (builder);
  for (guint i = 0; i < accounts->len; i++)
    account_to_json (builder, g_ptr_array_index (accounts, i));
  json_builder_end_array (builder);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  data = json_generator_to_data (generator, NULL);

  if (!g_file_set_contents (path, data, -1, &local_error))
    return secret_error (error, g_steal_pointer (&local_error));

  return TRUE;
}

gboolean
nuvashalauncher_secret_store_load (GPtrArray **accounts,
                                   char      **active_id,
                                   GError    **error)
{
  const char *store_file = test_secret_store_file ();
  g_autoptr (GError) local_error = NULL;
  g_autofree char *active = NULL;
  const SecretSchema *accounts_schema = account_schema_for_store ();
  const SecretSchema *active_account_schema = active_schema_for_store ();
  GList *items = NULL;
  GPtrArray *loaded = NULL;

  g_return_val_if_fail (accounts != NULL, FALSE);
  g_return_val_if_fail (active_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (store_file != NULL)
    return file_store_read (store_file, accounts, active_id, error);

  loaded = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_account_free);
  items = secret_password_search_sync (accounts_schema,
                                       SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS,
                                       NULL, &local_error,
                                       NULL);
  if (local_error != NULL)
    {
      g_ptr_array_unref (loaded);
      return secret_error (error, g_steal_pointer (&local_error));
    }

  for (GList *item = items; item != NULL; item = item->next)
    {
      SecretRetrievable *retrievable = SECRET_RETRIEVABLE (item->data);
      GHashTable *item_attrs = secret_retrievable_get_attributes (retrievable);

      if (item_attrs != NULL && g_hash_table_contains (item_attrs, "account-id"))
        {
          SecretValue *value = secret_retrievable_retrieve_secret_sync (retrievable, NULL, NULL);
          g_autoptr (NuvashaLauncherAccount) account =
            value != NULL ? account_from_payload (secret_value_get_text (value)) : NULL;

          if (value != NULL)
            secret_value_unref (value);

          if (account != NULL)
            g_ptr_array_add (loaded, g_steal_pointer (&account));
        }
    }
  g_list_free_full (items, g_object_unref);

  active = secret_password_lookup_sync (active_account_schema, NULL, &local_error,
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
nuvashalauncher_secret_store_save_account (const NuvashaLauncherAccount *account,
                                           GError **error)
{
  const char *store_file = test_secret_store_file ();
  g_autofree char *payload = NULL;
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (store_file != NULL)
    {
      g_autoptr (GPtrArray) accounts = NULL;
      g_autofree char *active_id = NULL;
      gboolean replaced = FALSE;

      if (!file_store_read (store_file, &accounts, &active_id, error))
        return FALSE;

      for (guint i = 0; i < accounts->len; i++)
        {
          NuvashaLauncherAccount *cur = g_ptr_array_index (accounts, i);

          if (g_strcmp0 (cur->id, account->id) == 0)
            {
              g_ptr_array_remove_index (accounts, i);
              g_ptr_array_insert (accounts, (gint) i,
                                  nuvashalauncher_account_copy (account));
              replaced = TRUE;
              break;
            }
        }
      if (!replaced)
        g_ptr_array_add (accounts, nuvashalauncher_account_copy (account));

      return file_store_write (store_file, accounts, active_id, error);
    }

  payload = account_payload (account);
  if (!secret_password_store_sync (account_schema_for_store (), SECRET_COLLECTION_DEFAULT,
                                   "Nuvasha Launcher account", payload, NULL,
                                   &local_error, "account-id", account->id, NULL))
    return secret_error (error, g_steal_pointer (&local_error));

  return TRUE;
}

gboolean
nuvashalauncher_secret_store_save_active (const char *account_id, GError **error)
{
  const char *store_file = test_secret_store_file ();
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (account_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (store_file != NULL)
    {
      g_autoptr (GPtrArray) accounts = NULL;
      g_autofree char *old_active = NULL;

      if (!file_store_read (store_file, &accounts, &old_active, error))
        return FALSE;
      return file_store_write (store_file, accounts, account_id, error);
    }

  if (!secret_password_store_sync (active_schema_for_store (), SECRET_COLLECTION_DEFAULT,
                                   "Nuvasha Launcher active account", account_id,
                                   NULL, &local_error, "kind", "active-account", NULL))
    return secret_error (error, g_steal_pointer (&local_error));
  return TRUE;
}

gboolean
nuvashalauncher_secret_store_remove_account (const char *account_id, GError **error)
{
  const char *store_file = test_secret_store_file ();
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (account_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (store_file != NULL)
    {
      g_autoptr (GPtrArray) accounts = NULL;
      g_autofree char *active_id = NULL;

      if (!file_store_read (store_file, &accounts, &active_id, error))
        return FALSE;

      for (guint i = 0; i < accounts->len; i++)
        {
          NuvashaLauncherAccount *cur = g_ptr_array_index (accounts, i);

          if (g_strcmp0 (cur->id, account_id) == 0)
            {
              g_ptr_array_remove_index (accounts, i);
              break;
            }
        }

      return file_store_write (store_file, accounts, active_id, error);
    }

  secret_password_clear_sync (account_schema_for_store (), NULL, &local_error,
                              "account-id", account_id, NULL);
  if (local_error != NULL)
    return secret_error (error, g_steal_pointer (&local_error));
  return TRUE;
}

gboolean
nuvashalauncher_secret_store_clear_active (GError **error)
{
  const char *store_file = test_secret_store_file ();
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (store_file != NULL)
    {
      g_autoptr (GPtrArray) accounts = NULL;
      g_autofree char *old_active = NULL;

      if (!file_store_read (store_file, &accounts, &old_active, error))
        return FALSE;
      return file_store_write (store_file, accounts, NULL, error);
    }

  secret_password_clear_sync (active_schema_for_store (), NULL, &local_error,
                              "kind", "active-account", NULL);
  if (local_error != NULL)
    return secret_error (error, g_steal_pointer (&local_error));
  return TRUE;
}
