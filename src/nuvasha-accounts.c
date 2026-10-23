/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-accounts.h"
#include "nuvasha-secret-store.h"
#include "nuvasha-fs.h"
#include "nuvasha-i18n.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define ACCOUNTS_FORMAT_VERSION 2

GQuark
nuvasha_accounts_error_quark (void)
{
  return g_quark_from_static_string ("nuvasha-accounts-error");
}

/* ---------------------------------------------------------- *
 * One account
 * ---------------------------------------------------------- */

void
nuvasha_account_free (NuvashaAccount *account)
{
  if (account == NULL)
    return;

  g_free (account->id);
  g_free (account->name);
  g_free (account->uuid);
  g_free (account->access_token);
  g_free (account->refresh_token);
  g_free (account->avatar_url);
  g_free (account->skin_model);
  g_free (account->cape_id);
  g_free (account);
}

NuvashaAccount *
nuvasha_account_copy (const NuvashaAccount *account)
{
  NuvashaAccount *copy = NULL;

  g_return_val_if_fail (account != NULL, NULL);

  copy = g_new0 (NuvashaAccount, 1);
  *copy = *account;
  copy->id = g_strdup (account->id);
  copy->name = g_strdup (account->name);
  copy->uuid = g_strdup (account->uuid);
  copy->access_token = g_strdup (account->access_token);
  copy->refresh_token = g_strdup (account->refresh_token);
  copy->avatar_url = g_strdup (account->avatar_url);
  copy->skin_model = g_strdup (account->skin_model);
  copy->cape_id = g_strdup (account->cape_id);

  return copy;
}

const char *
nuvasha_account_type_name (NuvashaAccountType type)
{
  return type == nuvasha_ACCOUNT_MICROSOFT ? _("Microsoft") : _("Offline");
}

char *
nuvasha_account_offline_uuid (const char *name)
{
  g_autofree char *seed = NULL;
  g_autoptr (GChecksum) checksum = NULL;
  guint8 digest[16];
  gsize length = sizeof digest;

  g_return_val_if_fail (name != NULL, NULL);

  seed = g_strconcat ("OfflinePlayer:", name, NULL);
  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, (const guchar *) seed, (gssize) strlen (seed));
  g_checksum_get_digest (checksum, digest, &length);

  digest[6] = (digest[6] & 0x0f) | 0x30; /* version 3 */
  digest[8] = (digest[8] & 0x3f) | 0x80; /* IETF variant */

  return g_strdup_printf (
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    digest[0], digest[1], digest[2], digest[3], digest[4], digest[5],
    digest[6], digest[7], digest[8], digest[9], digest[10], digest[11],
    digest[12], digest[13], digest[14], digest[15]);
}

/* Validate a player name using the game's allowed format. */
static gboolean
name_is_a_player_name (const char *name, const char **why)
{
  gsize length = 0;

  if (name == NULL || *name == '\0')
    {
      *why = _("a name cannot be empty");
      return FALSE;
    }

  length = strlen (name);

  if (length < 3 || length > 16)
    {
      *why = _("a name has to be 3 to 16 characters long");
      return FALSE;
    }

  for (gsize i = 0; i < length; i++)
    {
      if (g_ascii_isalnum (name[i]) || name[i] == '_')
        continue;

      *why = _("a name can only hold letters, digits and underscores");
      return FALSE;
    }

  return TRUE;
}

GType
nuvasha_account_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType type = g_boxed_type_register_static ("NuvashaAccount",
                                                (GBoxedCopyFunc) nuvasha_account_copy,
                                                (GBoxedFreeFunc) nuvasha_account_free);

      g_once_init_leave (&type_id, type);
    }

  return type_id;
}

NuvashaAccount *
nuvasha_account_new_offline (const char *name, GError **error)
{
  const char *why = NULL;
  NuvashaAccount *account = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!name_is_a_player_name (name, &why))
    {
      g_set_error (error, nuvasha_ACCOUNTS_ERROR, nuvasha_ACCOUNTS_ERROR_INVALID,
                   "%s", why);
      return NULL;
    }

  account = g_new0 (NuvashaAccount, 1);
  account->type = nuvasha_ACCOUNT_OFFLINE;
  account->id = g_strdup (name);
  account->name = g_strdup (name);
  account->uuid = nuvasha_account_offline_uuid (name);

  return account;
}

gboolean
nuvasha_account_token_is_valid (const NuvashaAccount *account, gint64 now)
{
  g_return_val_if_fail (account != NULL, FALSE);

  /* An offline account has no token and needs none. */
  if (account->type == nuvasha_ACCOUNT_OFFLINE)
    return TRUE;

  if (account->access_token == NULL || *account->access_token == '\0')
    return FALSE;

  if (account->expires_at == 0)
    return FALSE;

  return now < account->expires_at - nuvasha_ACCOUNTS_TOKEN_MARGIN;
}

/* ---------------------------------------------------------- *
 * The file
 * ---------------------------------------------------------- */

/* Serialize one account to the stored JSON format. */
static void
account_to_json (JsonBuilder *builder, const NuvashaAccount *account)
{
  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (
    builder, account->type == nuvasha_ACCOUNT_MICROSOFT ? "microsoft" : "offline");

  json_builder_set_member_name (builder, "id");
  json_builder_add_string_value (builder, account->id);

  json_builder_set_member_name (builder, "name");
  json_builder_add_string_value (builder, account->name);

  json_builder_set_member_name (builder, "uuid");
  json_builder_add_string_value (builder, account->uuid);

  if (account->type == nuvasha_ACCOUNT_MICROSOFT)
    {
      json_builder_set_member_name (builder, "expires-at");
      json_builder_add_int_value (builder, account->expires_at);
    }

  json_builder_set_member_name (builder, "avatar");
  if (account->avatar_url != NULL)
    json_builder_add_string_value (builder, account->avatar_url);
  else
    json_builder_add_null_value (builder);

  json_builder_set_member_name (builder, "skin-model");
  if (account->skin_model != NULL)
    json_builder_add_string_value (builder, account->skin_model);
  else
    json_builder_add_null_value (builder);

  json_builder_set_member_name (builder, "cape-id");
  if (account->cape_id != NULL)
    json_builder_add_string_value (builder, account->cape_id);
  else
    json_builder_add_null_value (builder);

  json_builder_end_object (builder);
}

/* The string member @name of @object, or NULL when it is not one. */
static char *
string_member (JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member (object, name);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;

  return g_strdup (json_node_get_string (node));
}

/* Parse one account entry, or NULL for an incomplete or invalid entry. */
static NuvashaAccount *
account_from_json (JsonObject *object)
{
  g_autofree char *type = NULL;
  g_autofree char *id = NULL;
  g_autofree char *name = NULL;
  g_autofree char *uuid = NULL;
  NuvashaAccount *account = NULL;

  type = string_member (object, "type");
  id = string_member (object, "id");
  name = string_member (object, "name");
  uuid = string_member (object, "uuid");

  if (type == NULL || id == NULL || *id == '\0' || name == NULL ||
      *name == '\0' || uuid == NULL || *uuid == '\0')
    return NULL;

  if (!g_str_equal (type, "microsoft") && !g_str_equal (type, "offline"))
    return NULL;

  account = g_new0 (NuvashaAccount, 1);
  account->type = g_str_equal (type, "microsoft") ? nuvasha_ACCOUNT_MICROSOFT
                                                  : nuvasha_ACCOUNT_OFFLINE;
  account->id = g_steal_pointer (&id);
  account->name = g_steal_pointer (&name);
  account->uuid = g_steal_pointer (&uuid);
  if (account->type == nuvasha_ACCOUNT_MICROSOFT)
    {
      g_autofree char *access_token = NULL;
      g_autofree char *refresh_token = NULL;

      if (nuvasha_secret_store_load (account->id, &access_token, &refresh_token))
        {
          account->access_token = g_steal_pointer (&access_token);
          account->refresh_token = g_steal_pointer (&refresh_token);
        }
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

/* ---------------------------------------------------------- *
 * The store
 * ---------------------------------------------------------- */

struct _NuvashaAccounts
{
  GObject parent_instance;

  char *filename;
  GPtrArray *accounts; /* NuvashaAccount*, element free nuvasha_account_free */
  char *active_id;
};

G_DEFINE_FINAL_TYPE (NuvashaAccounts, nuvasha_accounts, G_TYPE_OBJECT)

static guint nuvasha_accounts_signals[nuvasha_ACCOUNTS_N_SIGNALS];

static void
emit_changed (NuvashaAccounts *self)
{
  g_signal_emit (self, nuvasha_accounts_signals[nuvasha_ACCOUNTS_SIGNAL_CHANGED], 0);
}

NuvashaAccount *
nuvasha_accounts_find (NuvashaAccounts *self, const char *id)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), NULL);

  if (id == NULL)
    return NULL;

  for (guint i = 0; i < self->accounts->len; i++)
    {
      NuvashaAccount *account = g_ptr_array_index (self->accounts, i);

      if (g_str_equal (account->id, id))
        return account;
    }

  return NULL;
}

/* TRUE when @id is the one account that is active. */
static gboolean
active_is (NuvashaAccounts *self, const char *id)
{
  return self->active_id != NULL && g_str_equal (self->active_id, id);
}

/* Keep a file that cannot be understood so a user can recover it or report
 * it. The store starts empty, but never destroys the original evidence. */
static void
quarantine_corrupt_file (const char *path)
{
  g_autofree char *base = g_strconcat (path, ".corrupt", NULL);
  g_autofree char *corrupt_path = NULL;

  for (guint i = 0; i < 1000; i++)
    {
      corrupt_path = i == 0 ? g_strdup (base)
                            : g_strdup_printf ("%s.%u", base, i);
      if (!g_file_test (corrupt_path, G_FILE_TEST_EXISTS))
        break;
      g_clear_pointer (&corrupt_path, g_free);
    }

  if (corrupt_path == NULL ||
      !nuvasha_fs_rename (path, corrupt_path, NULL))
    g_warning ("could not quarantine the accounts file \"%s\": %s",
               path, g_strerror (errno));
  else
    g_warning ("the accounts file \"%s\" is corrupt; it was moved to \"%s\"",
               path, corrupt_path);
}

static gboolean
save (NuvashaAccounts *self, GError **error)
{
  g_autoptr (JsonBuilder) builder = NULL;
  g_autoptr (JsonGenerator) generator = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autofree char *directory = NULL;
  g_autofree char *data = NULL;

  for (guint i = 0; i < self->accounts->len; i++)
    {
      NuvashaAccount *account = g_ptr_array_index (self->accounts, i);

      if (account->type != nuvasha_ACCOUNT_MICROSOFT ||
          account->access_token == NULL || account->refresh_token == NULL)
        {
          if (account->type == nuvasha_ACCOUNT_MICROSOFT)
            nuvasha_secret_store_clear (account->id);
          continue;
        }

      if (!nuvasha_secret_store_save (account->id, account->access_token,
                                      account->refresh_token))
        {
          g_set_error (error, nuvasha_ACCOUNTS_ERROR,
                       nuvasha_ACCOUNTS_ERROR_IO,
                       "cannot store credentials for account \"%s\"",
                       account->name);
          return FALSE;
        }
    }

  builder = json_builder_new ();
  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "version");
  json_builder_add_int_value (builder, ACCOUNTS_FORMAT_VERSION);

  json_builder_set_member_name (builder, "accounts");
  json_builder_begin_array (builder);

  for (guint i = 0; i < self->accounts->len; i++)
    account_to_json (builder, g_ptr_array_index (self->accounts, i));

  json_builder_end_array (builder);

  json_builder_set_member_name (builder, "active");
  if (self->active_id != NULL)
    json_builder_add_string_value (builder, self->active_id);
  else
    json_builder_add_null_value (builder);

  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  json_generator_set_pretty (generator, TRUE);
  data = json_generator_to_data (generator, NULL);

  directory = g_path_get_dirname (self->filename);

  if (!nuvasha_fs_make_private_directory (directory, &local_error))
    {
      g_set_error (error, nuvasha_ACCOUNTS_ERROR, nuvasha_ACCOUNTS_ERROR_IO,
                   "%s", local_error->message);
      return FALSE;
    }

  /* Mode 0600 and written whole or not at all: account metadata remains
     private, and a save that is interrupted may never leave half of one
     behind. Credentials live in Secret Service, not in this file. */
  if (!nuvasha_fs_replace_contents (self->filename, data, -1, 0600,
                                &local_error))
    {
      g_prefix_error (&local_error, "cannot save \"%s\": ", self->filename);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

static void
load (NuvashaAccounts *self)
{
  g_autofree char *text = NULL;
  g_autoptr (JsonParser) parser = NULL;
  JsonNode *root = NULL;
  JsonNode *node = NULL;

  if (!nuvasha_fs_get_contents_within (self->filename,
                                   nuvasha_FS_MAX_SMALL_METADATA_SIZE,
                                   &text, NULL, NULL))
    return;

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, text, -1, NULL))
    {
      quarantine_corrupt_file (self->filename);
      return;
    }

  root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      quarantine_corrupt_file (self->filename);
      return;
    }

  node = json_object_get_member (json_node_get_object (root), "version");
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_INT64 &&
      json_node_get_int (node) > ACCOUNTS_FORMAT_VERSION)
    {
      quarantine_corrupt_file (self->filename);
      return;
    }

  node = json_object_get_member (json_node_get_object (root), "accounts");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          NuvashaAccount *account = NULL;

          if (!JSON_NODE_HOLDS_OBJECT (element))
            continue;

          account = account_from_json (json_node_get_object (element));

          /* Two entries of one identity are one account: the file wins
             over what is already here, and a duplicate is dropped. */
          if (account == NULL || nuvasha_accounts_find (self, account->id) != NULL)
            {
              nuvasha_account_free (account);
              continue;
            }

          g_ptr_array_add (self->accounts, account);
        }
    }

  {
    g_autofree char *active = string_member (json_node_get_object (root),
                                             "active");

    /* An active account that is not there is no active account at all. */
    if (active != NULL && nuvasha_accounts_find (self, active) != NULL)
      self->active_id = g_steal_pointer (&active);
  }

}

NuvashaAccounts *
nuvasha_accounts_new_for_path (const char *filename)
{
  NuvashaAccounts *self = NULL;

  g_return_val_if_fail (filename != NULL, NULL);

  self = g_object_new (nuvasha_TYPE_ACCOUNTS, NULL);
  self->filename = g_strdup (filename);
  self->accounts = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvasha_account_free);

  load (self);

  return self;
}

NuvashaAccounts *
nuvasha_accounts_new (void)
{
  g_autofree char *directory = g_build_filename (g_get_user_data_dir (), "nuvasha",
                                                 NULL);
  g_autofree char *filename = g_build_filename (directory, "accounts.json",
                                                NULL);

  return nuvasha_accounts_new_for_path (filename);
}

const char *
nuvasha_accounts_path (NuvashaAccounts *self)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), NULL);

  return self->filename;
}

GPtrArray *
nuvasha_accounts_list (NuvashaAccounts *self)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), NULL);

  return self->accounts;
}

guint
nuvasha_accounts_count (NuvashaAccounts *self)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), 0);

  return self->accounts->len;
}

NuvashaAccount *
nuvasha_accounts_get_active (NuvashaAccounts *self)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), NULL);

  if (self->active_id == NULL)
    return NULL;

  return nuvasha_accounts_find (self, self->active_id);
}

const char *
nuvasha_accounts_get_active_id (NuvashaAccounts *self)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), NULL);

  return self->active_id;
}

gboolean
nuvasha_accounts_set_active (NuvashaAccounts *self, const char *id, GError **error)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (nuvasha_accounts_find (self, id) == NULL)
    {
      g_set_error (error, nuvasha_ACCOUNTS_ERROR, nuvasha_ACCOUNTS_ERROR_UNKNOWN,
                   "there is no account \"%s\"", id != NULL ? id : "");
      return FALSE;
    }

  /* One active account, always: picking the one that is active changes
     nothing and is not a change worth telling a view about. */
  if (active_is (self, id))
    return TRUE;

  {
    g_autofree char *previous = g_steal_pointer (&self->active_id);

    self->active_id = g_strdup (id);

    if (!save (self, error))
      {
        g_free (self->active_id);
        self->active_id = g_steal_pointer (&previous);
        return FALSE;
      }
  }

  emit_changed (self);

  return TRUE;
}

gboolean
nuvasha_accounts_add (NuvashaAccounts *self, const NuvashaAccount *account, GError **error)
{
  NuvashaAccount *known = NULL;

  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (account->id == NULL || *account->id == '\0' ||
      account->name == NULL || *account->name == '\0' ||
      account->uuid == NULL || *account->uuid == '\0')
    {
      g_set_error (error, nuvasha_ACCOUNTS_ERROR, nuvasha_ACCOUNTS_ERROR_INVALID,
                   "an account needs a name, an id and a uuid to be stored");
      return FALSE;
    }

  known = nuvasha_accounts_find (self, account->id);

  if (known != NULL)
    {
      /* Signing in again replaces what was there, which is how a
         refreshed token and a changed profile are stored. */
      NuvashaAccount *replacement = nuvasha_account_copy (account);

      g_free (known->id);
      g_free (known->name);
      g_free (known->uuid);
      g_clear_pointer (&known->access_token, g_free);
      g_clear_pointer (&known->refresh_token, g_free);
      g_clear_pointer (&known->avatar_url, g_free);
      g_clear_pointer (&known->skin_model, g_free);
      g_clear_pointer (&known->cape_id, g_free);
      known->type = replacement->type;
      known->id = g_steal_pointer (&replacement->id);
      known->name = g_steal_pointer (&replacement->name);
      known->uuid = g_steal_pointer (&replacement->uuid);
      known->access_token = g_steal_pointer (&replacement->access_token);
      known->expires_at = replacement->expires_at;
      known->refresh_token = g_steal_pointer (&replacement->refresh_token);
      known->avatar_url = g_steal_pointer (&replacement->avatar_url);
      known->skin_model = g_steal_pointer (&replacement->skin_model);
      known->cape_id = g_steal_pointer (&replacement->cape_id);
      nuvasha_account_free (replacement);
    }
  else
    {
      g_ptr_array_add (self->accounts, nuvasha_account_copy (account));
    }

  /* A user who just signed in meant to play as that account: it becomes
     the active one, and with it the only active one there is. */
  g_free (self->active_id);
  self->active_id = g_strdup (account->id);

  if (!save (self, error))
    return FALSE;

  emit_changed (self);

  return TRUE;
}

gboolean
nuvasha_accounts_update (NuvashaAccounts *self, const NuvashaAccount *account,
                     GError **error)
{
  return nuvasha_accounts_add (self, account, error);
}

gboolean
nuvasha_accounts_remove (NuvashaAccounts *self, const char *id, GError **error)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (id == NULL)
    {
      g_set_error (error, nuvasha_ACCOUNTS_ERROR, nuvasha_ACCOUNTS_ERROR_UNKNOWN,
                   "there is no account to remove");
      return FALSE;
    }

  for (guint i = 0; i < self->accounts->len; i++)
    {
      NuvashaAccount *account = g_ptr_array_index (self->accounts, i);

      if (!g_str_equal (account->id, id))
        continue;

      g_ptr_array_remove_index (self->accounts, i);

      /* The account that was active is gone: what is left takes its
         place, and a store with nothing left has no active account. */
      if (active_is (self, id))
        {
          g_clear_pointer (&self->active_id, g_free);

          if (self->accounts->len > 0)
            self->active_id =
              g_strdup (((NuvashaAccount *) g_ptr_array_index (self->accounts, 0))
                          ->id);
        }

      if (!save (self, error))
        return FALSE;

      nuvasha_secret_store_clear (id);
      emit_changed (self);

      return TRUE;
    }

  g_set_error (error, nuvasha_ACCOUNTS_ERROR, nuvasha_ACCOUNTS_ERROR_UNKNOWN,
               "there is no account \"%s\" to remove", id != NULL ? id : "");

  return FALSE;
}

/* ---------------------------------------------------------- *
 * The GObject
 * ---------------------------------------------------------- */

static void
nuvasha_accounts_finalize (GObject *object)
{
  NuvashaAccounts *self = nuvasha_ACCOUNTS (object);

  g_clear_pointer (&self->accounts, g_ptr_array_unref);
  g_free (self->active_id);
  g_free (self->filename);

  G_OBJECT_CLASS (nuvasha_accounts_parent_class)->finalize (object);
}

static void
nuvasha_accounts_class_init (NuvashaAccountsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nuvasha_accounts_finalize;

  nuvasha_accounts_signals[nuvasha_ACCOUNTS_SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nuvasha_accounts_init (NuvashaAccounts *self G_GNUC_UNUSED)
{
  /* Everything is built by the constructors, which know the file. */
}
