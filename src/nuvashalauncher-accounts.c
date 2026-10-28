/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-accounts-private.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-secret-store.h"

#include <string.h>

GQuark
nuvashalauncher_accounts_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-accounts-error");
}

/* ---------------------------------------------------------- *
 * One account
 * ---------------------------------------------------------- */

void
nuvashalauncher_account_free (nuvashalauncherAccount *account)
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

nuvashalauncherAccount *
nuvashalauncher_account_copy (const nuvashalauncherAccount *account)
{
  nuvashalauncherAccount *copy = NULL;

  g_return_val_if_fail (account != NULL, NULL);

  copy = g_new0 (nuvashalauncherAccount, 1);
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
nuvashalauncher_account_type_name (nuvashalauncherAccountType type)
{
  return type == nuvashalauncher_ACCOUNT_MICROSOFT ? _("Microsoft") : _("Offline");
}

char *
nuvashalauncher_account_offline_uuid (const char *name)
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
nuvashalauncher_account_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType type = g_boxed_type_register_static ("nuvashalauncherAccount",
                                                (GBoxedCopyFunc) nuvashalauncher_account_copy,
                                                (GBoxedFreeFunc) nuvashalauncher_account_free);

      g_once_init_leave (&type_id, type);
    }

  return type_id;
}

nuvashalauncherAccount *
nuvashalauncher_account_new_offline (const char *name, GError **error)
{
  const char *why = NULL;
  nuvashalauncherAccount *account = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!name_is_a_player_name (name, &why))
    {
      g_set_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_INVALID,
                   "%s", why);
      return NULL;
    }

  account = g_new0 (nuvashalauncherAccount, 1);
  account->type = nuvashalauncher_ACCOUNT_OFFLINE;
  account->id = g_strdup (name);
  account->name = g_strdup (name);
  account->uuid = nuvashalauncher_account_offline_uuid (name);

  return account;
}

gboolean
nuvashalauncher_account_token_is_valid (const nuvashalauncherAccount *account, gint64 now)
{
  g_return_val_if_fail (account != NULL, FALSE);

  /* An offline account has no token and needs none. */
  if (account->type == nuvashalauncher_ACCOUNT_OFFLINE)
    return TRUE;

  if (account->access_token == NULL || *account->access_token == '\0')
    return FALSE;

  if (account->expires_at == 0)
    return FALSE;

  return now < account->expires_at - nuvashalauncher_ACCOUNTS_TOKEN_MARGIN;
}

/* ---------------------------------------------------------- *
 * Secret Service-backed store
 * ---------------------------------------------------------- */

struct _nuvashalauncherAccounts
{
  GObject parent_instance;

  char *filename; /* retained for the path-constructor API; never persisted */
  GPtrArray *accounts; /* nuvashalauncherAccount*, element free nuvashalauncher_account_free */
  char *active_id;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherAccounts, nuvashalauncher_accounts, G_TYPE_OBJECT)

static guint nuvashalauncher_accounts_signals[nuvashalauncher_ACCOUNTS_N_SIGNALS];

static void
emit_changed (nuvashalauncherAccounts *self)
{
  g_signal_emit (self, nuvashalauncher_accounts_signals[nuvashalauncher_ACCOUNTS_SIGNAL_CHANGED], 0);
}

static gboolean
active_is (nuvashalauncherAccounts *self, const char *id)
{
  return self->active_id != NULL && g_str_equal (self->active_id, id);
}

nuvashalauncherAccount *
nuvashalauncher_accounts_find (nuvashalauncherAccounts *self, const char *id)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), NULL);

  if (id == NULL)
    return NULL;

  for (guint i = 0; i < self->accounts->len; i++)
    {
      nuvashalauncherAccount *account = g_ptr_array_index (self->accounts, i);

      if (g_str_equal (account->id, id))
        return account;
    }

  return NULL;
}

static gboolean
save_account (const nuvashalauncherAccount *account, GError **error)
{
  return nuvashalauncher_secret_store_save_account (account, error);
}

static gboolean
save_active (const char *id, GError **error)
{
  return nuvashalauncher_secret_store_save_active (id, error);
}

nuvashalauncherAccounts *
nuvashalauncher_accounts_new_for_path (const char *filename)
{
  nuvashalauncherAccounts *self = NULL;

  g_return_val_if_fail (filename != NULL, NULL);

  self = g_object_new (nuvashalauncher_TYPE_ACCOUNTS, NULL);
  self->filename = g_strdup (filename);
  self->accounts = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_account_free);

  {
    g_autoptr (GPtrArray) loaded = NULL;
    g_autofree char *active = NULL;
    g_autoptr (GError) error = NULL;

    if (nuvashalauncher_secret_store_load (&loaded, &active, &error))
      {
        for (guint i = 0; i < loaded->len; i++)
          {
            nuvashalauncherAccount *account = g_ptr_array_index (loaded, i);

            if (nuvashalauncher_accounts_find (self, account->id) == NULL)
              g_ptr_array_add (self->accounts,
                               nuvashalauncher_account_copy (account));
          }

        if (active != NULL && nuvashalauncher_accounts_find (self, active) != NULL)
          self->active_id = g_steal_pointer (&active);
      }
    else if (error != NULL)
      /* The account API has no load-error return path. A Secret Service
       * provider is commonly unavailable in headless CLI/test environments;
       * treat that the same as an empty store and keep diagnostics quiet. A
       * later account write still reports the actual backend error. */
      g_debug ("cannot read accounts from Secret Service: %s", error->message);
  }

  return self;
}

nuvashalauncherAccounts *
nuvashalauncher_accounts_new (void)
{
  g_autofree char *directory = g_build_filename (g_get_user_data_dir (), "nuvashalauncher",
                                                 NULL);
  g_autofree char *filename = g_build_filename (directory, "accounts.json",
                                                NULL);

  return nuvashalauncher_accounts_new_for_path (filename);
}

const char *
nuvashalauncher_accounts_path (nuvashalauncherAccounts *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), NULL);

  return self->filename;
}

GPtrArray *
nuvashalauncher_accounts_list (nuvashalauncherAccounts *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), NULL);

  return self->accounts;
}

guint
nuvashalauncher_accounts_count (nuvashalauncherAccounts *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), 0);

  return self->accounts->len;
}

nuvashalauncherAccount *
nuvashalauncher_accounts_get_active (nuvashalauncherAccounts *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), NULL);

  if (self->active_id == NULL)
    return NULL;

  return nuvashalauncher_accounts_find (self, self->active_id);
}

const char *
nuvashalauncher_accounts_get_active_id (nuvashalauncherAccounts *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), NULL);

  return self->active_id;
}

gboolean
nuvashalauncher_accounts_set_active (nuvashalauncherAccounts *self, const char *id, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (nuvashalauncher_accounts_find (self, id) == NULL)
    {
      g_set_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN,
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

    if (!save_active (id, error))
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
nuvashalauncher_accounts_add (nuvashalauncherAccounts *self, const nuvashalauncherAccount *account, GError **error)
{
  nuvashalauncherAccount *known = NULL;
  nuvashalauncherAccount *previous = NULL;
  g_autofree char *previous_active_id = NULL;
  guint added_index = G_MAXUINT;

  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (account->id == NULL || *account->id == '\0' ||
      account->name == NULL || *account->name == '\0' ||
      account->uuid == NULL || *account->uuid == '\0')
    {
      g_set_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_INVALID,
                   "an account needs a name, an id and a uuid to be stored");
      return FALSE;
    }

  known = nuvashalauncher_accounts_find (self, account->id);
  previous_active_id = g_strdup (self->active_id);

  if (known != NULL)
    {
      /* Signing in again replaces what was there, which is how a
         refreshed token and a changed profile are stored. */
      previous = nuvashalauncher_account_copy (known);
      nuvashalauncherAccount *replacement = nuvashalauncher_account_copy (account);

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
      nuvashalauncher_account_free (replacement);
    }
  else
    {
      added_index = self->accounts->len;
      g_ptr_array_add (self->accounts, nuvashalauncher_account_copy (account));
    }

  /* A user who just signed in meant to play as that account: it becomes
     the active one, and with it the only active one there is. */
  g_free (self->active_id);
  self->active_id = g_strdup (account->id);

  if (!save_account (account, error) || !save_active (account->id, error))
    {
      if (known != NULL)
        save_account (previous, NULL);
      else
        nuvashalauncher_secret_store_remove_account (account->id, NULL);
      if (previous_active_id != NULL)
        save_active (previous_active_id, NULL);
      else
        nuvashalauncher_secret_store_clear_active (NULL);

      if (known != NULL)
        {
          g_free (known->id);
          g_free (known->name);
          g_free (known->uuid);
          g_free (known->access_token);
          g_free (known->refresh_token);
          g_free (known->avatar_url);
          g_free (known->skin_model);
          g_free (known->cape_id);
          known->type = previous->type;
          known->id = g_steal_pointer (&previous->id);
          known->name = g_steal_pointer (&previous->name);
          known->uuid = g_steal_pointer (&previous->uuid);
          known->access_token = g_steal_pointer (&previous->access_token);
          known->expires_at = previous->expires_at;
          known->refresh_token = g_steal_pointer (&previous->refresh_token);
          known->avatar_url = g_steal_pointer (&previous->avatar_url);
          known->skin_model = g_steal_pointer (&previous->skin_model);
          known->cape_id = g_steal_pointer (&previous->cape_id);
        }
      else if (added_index != G_MAXUINT)
        g_ptr_array_remove_index (self->accounts, added_index);

      g_clear_pointer (&self->active_id, g_free);
      self->active_id = g_steal_pointer (&previous_active_id);
      g_clear_pointer (&previous, nuvashalauncher_account_free);
      return FALSE;
    }

  g_clear_pointer (&previous, nuvashalauncher_account_free);

  emit_changed (self);

  return TRUE;
}

gboolean
nuvashalauncher_accounts_update (nuvashalauncherAccounts *self, const nuvashalauncherAccount *account,
                     GError **error)
{
  return nuvashalauncher_accounts_add (self, account, error);
}

gboolean
nuvashalauncher_accounts_remove (nuvashalauncherAccounts *self, const char *id, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_IS_ACCOUNTS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (id == NULL)
    {
      g_set_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN,
                   "there is no account to remove");
      return FALSE;
    }

  for (guint i = 0; i < self->accounts->len; i++)
    {
      nuvashalauncherAccount *account = g_ptr_array_index (self->accounts, i);
      g_autofree char *old_active_id = NULL;
      if (!g_str_equal (account->id, id))
        continue;

      old_active_id = g_strdup (self->active_id);
      account = g_ptr_array_steal_index (self->accounts, i);

      /* The account that was active is gone: what is left takes its
         place, and a store with nothing left has no active account. */
      if (active_is (self, id))
        {
          g_clear_pointer (&self->active_id, g_free);

          if (self->accounts->len > 0)
            self->active_id =
              g_strdup (((nuvashalauncherAccount *) g_ptr_array_index (self->accounts, 0))
                          ->id);
        }

      if (!nuvashalauncher_secret_store_remove_account (id, error) ||
          (old_active_id != NULL && g_str_equal (old_active_id, id) &&
           (self->active_id != NULL
              ? !save_active (self->active_id, error)
              : !nuvashalauncher_secret_store_clear_active (error))))
        {
          /* Restore both memory and the keyring entry if the second half of
           * the removal could not be committed. */
          nuvashalauncher_secret_store_save_account (account, NULL);
          if (old_active_id != NULL)
            save_active (old_active_id, NULL);
          g_ptr_array_insert (self->accounts, i, account);
          g_free (self->active_id);
          self->active_id = g_steal_pointer (&old_active_id);
          return FALSE;
        }

      nuvashalauncher_account_free (account);
      emit_changed (self);

      return TRUE;
    }

  g_set_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN,
               "there is no account \"%s\" to remove", id != NULL ? id : "");

  return FALSE;
}

/* ---------------------------------------------------------- *
 * The GObject
 * ---------------------------------------------------------- */

static void
nuvashalauncher_accounts_finalize (GObject *object)
{
  nuvashalauncherAccounts *self = nuvashalauncher_ACCOUNTS (object);

  g_clear_pointer (&self->accounts, g_ptr_array_unref);
  g_free (self->active_id);
  g_free (self->filename);

  G_OBJECT_CLASS (nuvashalauncher_accounts_parent_class)->finalize (object);
}

static void
nuvashalauncher_accounts_class_init (nuvashalauncherAccountsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nuvashalauncher_accounts_finalize;

  nuvashalauncher_accounts_signals[nuvashalauncher_ACCOUNTS_SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nuvashalauncher_accounts_init (nuvashalauncherAccounts *self G_GNUC_UNUSED)
{
  /* Everything is built by the constructors, which know the file. */
}
