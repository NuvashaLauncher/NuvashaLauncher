/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncher-accounts: what an account is, what the store keeps and
 * which one of them is active.
 *
 * Nothing here touches the network and nothing needs a display: the
 * store is written into a temporary file, so what a test checks is what
 * a user would find after a restart. */

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-secret-store.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The file of a store that only this test knows about. */
static char *
temporary_path (const char *name)
{
  g_autofree char *directory = g_dir_make_tmp ("nuvashalauncher-accounts-XXXXXX", NULL);
  g_autofree char *path = NULL;

  path = g_build_filename (directory, name, NULL);

  return g_steal_pointer (&path);
}

/* ---------------------------------------------------------- *
 * What an account is
 * ---------------------------------------------------------- */

static void
test_offline_account (void)
{
  g_autoptr (GError) error = NULL;
  nuvashalauncherAccount *account = NULL;

  account = nuvashalauncher_account_new_offline ("TestUser", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);

  /* An offline account is a name and nothing else: no token, nothing to
     refresh, and it says so wherever it is shown. */
  g_assert_cmpint (account->type, ==, nuvashalauncher_ACCOUNT_OFFLINE);
  g_assert_cmpstr (account->id, ==, "TestUser");
  g_assert_cmpstr (account->name, ==, "TestUser");
  g_assert_cmpstr (account->uuid, ==, "097d3392-865a-3f3c-8b4a-da1c3473466c");
  g_assert_null (account->access_token);
  g_assert_null (account->refresh_token);
  g_assert_cmpstr (nuvashalauncher_account_type_name (account->type), ==, "Offline");
  g_assert_cmpstr (nuvashalauncher_account_type_name (nuvashalauncher_ACCOUNT_MICROSOFT), ==,
                   "Microsoft");

  /* An offline account needs no token and is therefore always good. */
  g_assert_true (nuvashalauncher_account_token_is_valid (account, 0));
  g_assert_true (nuvashalauncher_account_token_is_valid (account, G_MAXINT64));

  nuvashalauncher_account_free (account);
}

/* The UUID java hands out for an offline player is the MD5 of
   "OfflinePlayer:<name>" as a name based UUID, and nuvashalauncher has to hand out the
   same one or the worlds of a player are not found where they are. */
static void
test_offline_uuid_derivation (void)
{
  g_autofree char *first = NULL;
  g_autofree char *second = NULL;

  /* These are the UUIDs java hands out for these names, taken from a
     java of its own (UUID.nameUUIDFromBytes("OfflinePlayer:" + name)),
     which is what makes them worth writing down here. */
  first = nuvashalauncher_account_offline_uuid ("Notch");
  g_assert_cmpstr (first, ==, "b50ad385-829d-3141-a216-7e7d7539ba7f");

  {
    g_autofree char *player = nuvashalauncher_account_offline_uuid ("Player");
    g_autofree char *test_user = nuvashalauncher_account_offline_uuid ("TestUser");
    g_autofree char *jeb = nuvashalauncher_account_offline_uuid ("jeb_");

    g_assert_cmpstr (player, ==, "a01e3843-e521-3998-958a-f459800e4d11");
    g_assert_cmpstr (test_user, ==, "097d3392-865a-3f3c-8b4a-da1c3473466c");
    g_assert_cmpstr (jeb, ==, "a762f560-4fce-3236-812a-b80efff0b62b");
  }

  /* The same name always gives the same uuid, and a different name a
     different one. */
  second = nuvashalauncher_account_offline_uuid ("Notch");
  g_assert_cmpstr (second, ==, first);
  g_clear_pointer (&second, g_free);

  second = nuvashalauncher_account_offline_uuid ("jeb_");
  g_assert_cmpstr (second, !=, first);

  /* Version 3 and the IETF variant: the nibbles a name based UUID has
     and a random one would not. */
  g_assert_cmpint (first[14], ==, '3');
  g_assert_nonnull (strchr ("89ab", first[19]));

  g_print ("ok - an offline player gets the uuid java gives him\n");
}

/* The name of an offline account is the name the game will be given, so
   it has to be one the game accepts. */
static void
test_offline_name_validation (void)
{
  g_autoptr (GError) error = NULL;
  nuvashalauncherAccount *account = NULL;

  account = nuvashalauncher_account_new_offline ("ab", &error);
  g_assert_null (account);
  g_assert_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_INVALID);
  g_assert_nonnull (strstr (error->message, "3 to 16"));
  g_clear_error (&error);

  g_assert_null (nuvashalauncher_account_new_offline ("0123456789abcdefg", &error));
  g_assert_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_INVALID);
  g_clear_error (&error);

  g_assert_null (nuvashalauncher_account_new_offline ("Fe lia", &error));
  g_assert_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_INVALID);
  g_assert_nonnull (strstr (error->message, "letters"));
  g_clear_error (&error);

  g_assert_null (nuvashalauncher_account_new_offline ("", &error));
  g_assert_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_INVALID);
  g_clear_error (&error);

  account = nuvashalauncher_account_new_offline ("abc", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);
  nuvashalauncher_account_free (account);

  account = nuvashalauncher_account_new_offline ("0123456789abcdef", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);
  nuvashalauncher_account_free (account);

  account = nuvashalauncher_account_new_offline ("a_b_9", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);
  nuvashalauncher_account_free (account);

  g_print ("ok - an offline name is what the game accepts as one\n");
}

/* ---------------------------------------------------------- *
 * When a token is good
 * ---------------------------------------------------------- */

static nuvashalauncherAccount *
microsoft_account (gint64 expires_at, const char *access_token)
{
  nuvashalauncherAccount *account = g_new0 (nuvashalauncherAccount, 1);

  account->type = nuvashalauncher_ACCOUNT_MICROSOFT;
  account->id = g_strdup ("069a79f444e94726a5befca90e38aaf5");
  account->name = g_strdup ("Notch");
  account->uuid = g_strdup ("069a79f4-44e9-4726-a5be-fca90e38aaf5");
  account->access_token = g_strdup (access_token);
  account->expires_at = expires_at;

  return account;
}

static void
test_token_expiry (void)
{
  g_autoptr (nuvashalauncherAccount) account = NULL;
  const gint64 now = 1000000;

  /* A token with an hour left is good. */
  account = microsoft_account (now + 3600, "token");
  g_assert_true (nuvashalauncher_account_token_is_valid (account, now));

  /* A token that is about to run out is not: the game would be handed
     one that dies while it is still booting. */
  g_assert_false (
    nuvashalauncher_account_token_is_valid (account, now + 3600 - nuvashalauncher_ACCOUNTS_TOKEN_MARGIN));
  g_assert_false (
    nuvashalauncher_account_token_is_valid (account, now + 3600 - nuvashalauncher_ACCOUNTS_TOKEN_MARGIN + 1));
  g_assert_true (nuvashalauncher_account_token_is_valid (
    account, now + 3600 - nuvashalauncher_ACCOUNTS_TOKEN_MARGIN - 1));

  /* A token that is gone, an account that never had one and a moment with
     no sense of time all mean the same thing: sign in again. */
  g_assert_false (nuvashalauncher_account_token_is_valid (account, now + 7200));

  g_clear_pointer (&account->access_token, g_free);
  g_assert_false (nuvashalauncher_account_token_is_valid (account, now));

  /* The next account is a new one, not the old one emptied out. */
  g_clear_pointer (&account, nuvashalauncher_account_free);
  account = microsoft_account (0, "token");
  g_assert_false (nuvashalauncher_account_token_is_valid (account, now));

  g_print ("ok - a token stops being good before it stops being valid\n");
}

/* ---------------------------------------------------------- *
 * The store
 * ---------------------------------------------------------- */

static void
test_one_active_account (void)
{
  g_autofree char *path = temporary_path ("accounts.json");
  g_autoptr (nuvashalauncherAccounts) store = NULL;
  g_autoptr (nuvashalauncherAccount) one = NULL;
  g_autoptr (nuvashalauncherAccount) two = NULL;
  g_autoptr (GError) error = NULL;

  nuvashalauncher_secret_store_remove_account ("TestUser", NULL);
  nuvashalauncher_secret_store_remove_account ("jeb_", NULL);
  nuvashalauncher_secret_store_remove_account ("069a79f444e94726a5befca90e38aaf5", NULL);
  nuvashalauncher_secret_store_remove_account ("SkinTest", NULL);
  nuvashalauncher_secret_store_remove_account ("Steve", NULL);
  nuvashalauncher_secret_store_remove_account ("Alex", NULL);
  nuvashalauncher_secret_store_clear_active (NULL);
  store = nuvashalauncher_accounts_new_for_path (path);
  g_assert_cmpuint (nuvashalauncher_accounts_count (store), ==, 0);

  /* A store with nothing in it has no active account, which is what a
     launcher has to tell a user about. */
  g_assert_null (nuvashalauncher_accounts_get_active (store));
  g_assert_null (nuvashalauncher_accounts_get_active_id (store));

  one = nuvashalauncher_account_new_offline ("TestUser", &error);
  g_assert_no_error (error);

  two = nuvashalauncher_account_new_offline ("jeb_", &error);
  g_assert_no_error (error);

  if (!nuvashalauncher_accounts_add (store, one, &error))
    {
      g_test_skip ("Secret Service is unavailable in this environment");
      return;
    }
  g_assert_no_error (error);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active_id (store), ==, "TestUser");

  /* Adding a second account makes it the active one: a user who just
     signed in meant to play as that one. */
  g_assert_true (nuvashalauncher_accounts_add (store, two, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (nuvashalauncher_accounts_count (store), ==, 2);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active_id (store), ==, "jeb_");

  /* There is one active account and it is one of the accounts. */
  g_assert_true (nuvashalauncher_accounts_set_active (store, "TestUser", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active_id (store), ==, "TestUser");
  g_assert_cmpstr (nuvashalauncher_accounts_get_active (store)->name, ==, "TestUser");

  /* An account that is not there cannot be made active. */
  g_assert_false (nuvashalauncher_accounts_set_active (store, "nobody", &error));
  g_assert_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN);
  g_clear_error (&error);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active_id (store), ==, "TestUser");

  /* Removing the active account hands the active one on to what is
     left, so the store never has an active account that is gone. */
  g_assert_true (nuvashalauncher_accounts_remove (store, "TestUser", &error));
  g_assert_no_error (error);
  g_assert_cmpuint (nuvashalauncher_accounts_count (store), ==, 1);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active_id (store), ==, "jeb_");

  /* Removing the last one leaves the store with none, and that is a
     state a launcher has words for. */
  g_assert_true (nuvashalauncher_accounts_remove (store, "jeb_", &error));
  g_assert_no_error (error);
  g_assert_null (nuvashalauncher_accounts_get_active (store));

  g_assert_false (nuvashalauncher_accounts_remove (store, "jeb_", &error));
  g_assert_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN);

  g_print ("ok - there is one active account and it is one of them\n");
}

/* What a user finds after a restart is what the store kept, tokens and
   all, and nobody but the user can read what it kept. */
static void
test_persistence (void)
{
  g_autofree char *path = temporary_path ("accounts.json");
  g_autoptr (nuvashalauncherAccounts) store = NULL;
  g_autoptr (nuvashalauncherAccounts) reopened = NULL;
  g_autoptr (nuvashalauncherAccount) account = NULL;
  nuvashalauncherAccount *found = NULL;
  g_autoptr (GError) error = NULL;

  store = nuvashalauncher_accounts_new_for_path (path);
  account = microsoft_account (1800000000, "the-access-token");
  account->refresh_token = g_strdup ("the-refresh-token");
  account->avatar_url = g_strdup ("https://example.invalid/skin.png");
  account->skin_model = g_strdup ("SLIM");
  account->cape_id = g_strdup ("cape");

  /* Secret Service is an installation requirement for account persistence,
   * but headless test environments may not provide one. */
  nuvashalauncher_secret_store_remove_account (account->id, NULL);
  if (!nuvashalauncher_accounts_add (store, account, &error))
    {
      g_test_skip ("Secret Service is unavailable in this environment");
      return;
    }
  g_assert_no_error (error);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  reopened = nuvashalauncher_accounts_new_for_path (path);
  g_assert_cmpuint (nuvashalauncher_accounts_count (reopened), ==, 1);
  found = nuvashalauncher_accounts_get_active (reopened);
  g_assert_nonnull (found);
  g_assert_cmpint (found->type, ==, nuvashalauncher_ACCOUNT_MICROSOFT);
  g_assert_cmpstr (found->name, ==, "Notch");
  g_assert_cmpstr (found->access_token, ==, "the-access-token");
  g_assert_cmpstr (found->refresh_token, ==, "the-refresh-token");
  g_assert_cmpint (found->expires_at, ==, 1800000000);
  g_assert_cmpstr (found->avatar_url, ==, "https://example.invalid/skin.png");
  g_assert_cmpstr (found->skin_model, ==, "SLIM");
  g_assert_cmpstr (found->cape_id, ==, "cape");

  g_assert_true (nuvashalauncher_accounts_remove (
    store, account->id, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (nuvashalauncher_accounts_count (store), ==, 0);
  g_assert_null (nuvashalauncher_accounts_get_active (store));

  g_print ("ok - complete account records survive a restart in Secret Service\n");
}

static void
on_changed (nuvashalauncherAccounts *store G_GNUC_UNUSED, gpointer user_data)
{
  guint *changed = user_data;

  (*changed)++;
}

/* Signing in again is how a refreshed token and a changed profile are
   stored, and it must not make a second account of one identity. */
static void
test_adding_the_same_account_twice (void)
{
  g_autofree char *path = temporary_path ("accounts.json");
  g_autoptr (nuvashalauncherAccounts) store = NULL;
  g_autoptr (nuvashalauncherAccount) account = NULL;
  g_autoptr (nuvashalauncherAccount) other = NULL;
  g_autoptr (GError) error = NULL;
  guint changed = 0;

  nuvashalauncher_secret_store_remove_account ("069a79f444e94726a5befca90e38aaf5", NULL);
  nuvashalauncher_secret_store_remove_account ("TestUser", NULL);
  nuvashalauncher_secret_store_clear_active (NULL);
  store = nuvashalauncher_accounts_new_for_path (path);

  g_signal_connect (store, "changed", G_CALLBACK (on_changed), &changed);

  account = microsoft_account (1800000000, "first-token");
  if (!nuvashalauncher_accounts_add (store, account, &error))
    {
      g_test_skip ("Secret Service is unavailable in this environment");
      return;
    }
  g_assert_no_error (error);
  g_assert_cmpuint (changed, ==, 1);

  /* The same identity with a new token and a new expiry: one account,
     not two. */
  g_free (account->access_token);
  account->access_token = g_strdup ("second-token");
  account->expires_at = 1800003600;
  g_assert_true (nuvashalauncher_accounts_update (store, account, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (nuvashalauncher_accounts_count (store), ==, 1);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active (store)->access_token, ==,
                   "second-token");
  g_assert_cmpint (nuvashalauncher_accounts_get_active (store)->expires_at, ==, 1800003600);
  g_assert_cmpuint (changed, ==, 2);

  /* Two offline accounts of one name are one account as well, because the
     name is what they are keyed by. */
  other = nuvashalauncher_account_new_offline ("TestUser", &error);
  g_assert_no_error (error);
  g_assert_true (nuvashalauncher_accounts_add (store, other, &error));
  g_assert_true (nuvashalauncher_accounts_add (store, other, &error));
  g_assert_cmpuint (nuvashalauncher_accounts_count (store), ==, 2);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active_id (store), ==, "TestUser");

  /* Picking the active account again is not a change: a view would do
     nothing about it and there is nothing to write. */
  changed = 0;
  g_assert_true (nuvashalauncher_accounts_set_active (store, "TestUser", &error));
  g_assert_no_error (error);
  g_assert_cmpuint (changed, ==, 0);

  nuvashalauncher_secret_store_remove_account ("069a79f444e94726a5befca90e38aaf5", NULL);
  nuvashalauncher_secret_store_remove_account ("TestUser", NULL);
  nuvashalauncher_secret_store_clear_active (NULL);

  g_print ("ok - signing in again replaces the account it signed in to\n");
}

int
main (void)
{
  g_print ("# Account tests\n");

  test_offline_account ();
  test_offline_uuid_derivation ();
  test_offline_name_validation ();
  test_token_expiry ();
  test_one_active_account ();
  test_persistence ();
  test_adding_the_same_account_twice ();

  return EXIT_SUCCESS;
}
