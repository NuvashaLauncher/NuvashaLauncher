/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for comfy-accounts: what an account is, what the store keeps and
 * which one of them is active.
 *
 * Nothing here touches the network and nothing needs a display: the
 * store is written into a temporary file, so what a test checks is what
 * a user would find after a restart. */

#include "comfy-accounts.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The file of a store that only this test knows about. */
static char *
temporary_path (const char *name)
{
  g_autofree char *directory = g_dir_make_tmp ("comfy-accounts-XXXXXX", NULL);
  g_autofree char *path = NULL;

  path = g_build_filename (directory, name, NULL);

  return g_steal_pointer (&path);
}

static void
check_permission (const char *path, int mode)
{
  GStatBuf info = { 0 };

  g_assert_cmpint (g_stat (path, &info), ==, 0);
  g_assert_cmpint (info.st_mode & 0777, ==, mode);
}

/* ---------------------------------------------------------- *
 * What an account is
 * ---------------------------------------------------------- */

static void
test_offline_account (void)
{
  g_autoptr (GError) error = NULL;
  ComfyAccount *account = NULL;

  account = comfy_account_new_offline ("Flia", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);

  /* An offline account is a name and nothing else: no token, nothing to
     refresh, and it says so wherever it is shown. */
  g_assert_cmpint (account->type, ==, comfy_ACCOUNT_OFFLINE);
  g_assert_cmpstr (account->id, ==, "Flia");
  g_assert_cmpstr (account->name, ==, "Flia");
  g_assert_cmpstr (account->uuid, ==, "41b05633-42e0-3a6f-b64b-6b64448dccdd");
  g_assert_null (account->access_token);
  g_assert_null (account->refresh_token);
  g_assert_cmpstr (comfy_account_type_name (account->type), ==, "Offline");
  g_assert_cmpstr (comfy_account_type_name (comfy_ACCOUNT_MICROSOFT), ==,
                   "Microsoft");

  /* An offline account needs no token and is therefore always good. */
  g_assert_true (comfy_account_token_is_valid (account, 0));
  g_assert_true (comfy_account_token_is_valid (account, G_MAXINT64));

  comfy_account_free (account);
}

/* The UUID java hands out for an offline player is the MD5 of
   "OfflinePlayer:<name>" as a name based UUID, and comfy has to hand out the
   same one or the worlds of a player are not found where they are. */
static void
test_offline_uuid_derivation (void)
{
  g_autofree char *first = NULL;
  g_autofree char *second = NULL;

  /* These are the UUIDs java hands out for these names, taken from a
     java of its own (UUID.nameUUIDFromBytes("OfflinePlayer:" + name)),
     which is what makes them worth writing down here. */
  first = comfy_account_offline_uuid ("Notch");
  g_assert_cmpstr (first, ==, "b50ad385-829d-3141-a216-7e7d7539ba7f");

  {
    g_autofree char *player = comfy_account_offline_uuid ("Player");
    g_autofree char *flia = comfy_account_offline_uuid ("Flia");
    g_autofree char *jeb = comfy_account_offline_uuid ("jeb_");

    g_assert_cmpstr (player, ==, "a01e3843-e521-3998-958a-f459800e4d11");
    g_assert_cmpstr (flia, ==, "41b05633-42e0-3a6f-b64b-6b64448dccdd");
    g_assert_cmpstr (jeb, ==, "a762f560-4fce-3236-812a-b80efff0b62b");
  }

  /* The same name always gives the same uuid, and a different name a
     different one. */
  second = comfy_account_offline_uuid ("Notch");
  g_assert_cmpstr (second, ==, first);
  g_clear_pointer (&second, g_free);

  second = comfy_account_offline_uuid ("jeb_");
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
  ComfyAccount *account = NULL;

  account = comfy_account_new_offline ("ab", &error);
  g_assert_null (account);
  g_assert_error (error, comfy_ACCOUNTS_ERROR, comfy_ACCOUNTS_ERROR_INVALID);
  g_assert_nonnull (strstr (error->message, "3 to 16"));
  g_clear_error (&error);

  g_assert_null (comfy_account_new_offline ("0123456789abcdefg", &error));
  g_assert_error (error, comfy_ACCOUNTS_ERROR, comfy_ACCOUNTS_ERROR_INVALID);
  g_clear_error (&error);

  g_assert_null (comfy_account_new_offline ("Fe lia", &error));
  g_assert_error (error, comfy_ACCOUNTS_ERROR, comfy_ACCOUNTS_ERROR_INVALID);
  g_assert_nonnull (strstr (error->message, "letters"));
  g_clear_error (&error);

  g_assert_null (comfy_account_new_offline ("", &error));
  g_assert_error (error, comfy_ACCOUNTS_ERROR, comfy_ACCOUNTS_ERROR_INVALID);
  g_clear_error (&error);

  /* The edges of what is allowed are allowed. */
  account = comfy_account_new_offline ("abc", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);
  comfy_account_free (account);

  account = comfy_account_new_offline ("0123456789abcdef", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);
  comfy_account_free (account);

  account = comfy_account_new_offline ("a_b_9", &error);
  g_assert_no_error (error);
  g_assert_nonnull (account);
  comfy_account_free (account);

  g_print ("ok - an offline name is what the game accepts as one\n");
}

/* ---------------------------------------------------------- *
 * When a token is good
 * ---------------------------------------------------------- */

static ComfyAccount *
microsoft_account (gint64 expires_at, const char *access_token)
{
  ComfyAccount *account = g_new0 (ComfyAccount, 1);

  account->type = comfy_ACCOUNT_MICROSOFT;
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
  g_autoptr (ComfyAccount) account = NULL;
  const gint64 now = 1000000;

  /* A token with an hour left is good. */
  account = microsoft_account (now + 3600, "token");
  g_assert_true (comfy_account_token_is_valid (account, now));

  /* A token that is about to run out is not: the game would be handed
     one that dies while it is still booting. */
  g_assert_false (
    comfy_account_token_is_valid (account, now + 3600 - comfy_ACCOUNTS_TOKEN_MARGIN));
  g_assert_false (
    comfy_account_token_is_valid (account, now + 3600 - comfy_ACCOUNTS_TOKEN_MARGIN + 1));
  g_assert_true (comfy_account_token_is_valid (
    account, now + 3600 - comfy_ACCOUNTS_TOKEN_MARGIN - 1));

  /* A token that is gone, an account that never had one and a moment with
     no sense of time all mean the same thing: sign in again. */
  g_assert_false (comfy_account_token_is_valid (account, now + 7200));

  g_clear_pointer (&account->access_token, g_free);
  g_assert_false (comfy_account_token_is_valid (account, now));

  /* The next account is a new one, not the old one emptied out. */
  g_clear_pointer (&account, comfy_account_free);
  account = microsoft_account (0, "token");
  g_assert_false (comfy_account_token_is_valid (account, now));

  g_print ("ok - a token stops being good before it stops being valid\n");
}

/* ---------------------------------------------------------- *
 * The store
 * ---------------------------------------------------------- */

static void
test_one_active_account (void)
{
  g_autofree char *path = temporary_path ("accounts.json");
  g_autoptr (ComfyAccounts) store = NULL;
  g_autoptr (ComfyAccount) one = NULL;
  g_autoptr (ComfyAccount) two = NULL;
  g_autoptr (GError) error = NULL;

  store = comfy_accounts_new_for_path (path);
  g_assert_cmpuint (comfy_accounts_count (store), ==, 0);

  /* A store with nothing in it has no active account, which is what a
     launcher has to tell a user about. */
  g_assert_null (comfy_accounts_get_active (store));
  g_assert_null (comfy_accounts_get_active_id (store));

  one = comfy_account_new_offline ("Flia", &error);
  g_assert_no_error (error);

  two = comfy_account_new_offline ("jeb_", &error);
  g_assert_no_error (error);

  g_assert_true (comfy_accounts_add (store, one, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (comfy_accounts_get_active_id (store), ==, "Flia");

  /* Adding a second account makes it the active one: a user who just
     signed in meant to play as that one. */
  g_assert_true (comfy_accounts_add (store, two, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (comfy_accounts_count (store), ==, 2);
  g_assert_cmpstr (comfy_accounts_get_active_id (store), ==, "jeb_");

  /* There is one active account and it is one of the accounts. */
  g_assert_true (comfy_accounts_set_active (store, "Flia", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (comfy_accounts_get_active_id (store), ==, "Flia");
  g_assert_cmpstr (comfy_accounts_get_active (store)->name, ==, "Flia");

  /* An account that is not there cannot be made active. */
  g_assert_false (comfy_accounts_set_active (store, "nobody", &error));
  g_assert_error (error, comfy_ACCOUNTS_ERROR, comfy_ACCOUNTS_ERROR_UNKNOWN);
  g_clear_error (&error);
  g_assert_cmpstr (comfy_accounts_get_active_id (store), ==, "Flia");

  /* Removing the active account hands the active one on to what is
     left, so the store never has an active account that is gone. */
  g_assert_true (comfy_accounts_remove (store, "Flia", &error));
  g_assert_no_error (error);
  g_assert_cmpuint (comfy_accounts_count (store), ==, 1);
  g_assert_cmpstr (comfy_accounts_get_active_id (store), ==, "jeb_");

  /* Removing the last one leaves the store with none, and that is a
     state a launcher has words for. */
  g_assert_true (comfy_accounts_remove (store, "jeb_", &error));
  g_assert_no_error (error);
  g_assert_null (comfy_accounts_get_active (store));

  g_assert_false (comfy_accounts_remove (store, "jeb_", &error));
  g_assert_error (error, comfy_ACCOUNTS_ERROR, comfy_ACCOUNTS_ERROR_UNKNOWN);

  g_print ("ok - there is one active account and it is one of them\n");
}

/* What a user finds after a restart is what the store kept, tokens and
   all, and nobody but the user can read what it kept. */
static void
test_persistence (void)
{
  g_autofree char *path = temporary_path ("accounts.json");
  g_autoptr (ComfyAccounts) store = NULL;
  g_autoptr (ComfyAccounts) reopened = NULL;
  g_autoptr (ComfyAccount) account = NULL;
  ComfyAccount *found = NULL; /* the store keeps it, so this borrows it */
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;

  store = comfy_accounts_new_for_path (path);
  g_assert_cmpstr (comfy_accounts_path (store), ==, path);

  account = microsoft_account (1800000000, "the-access-token");
  account->refresh_token = g_strdup ("the-refresh-token");
  account->avatar_url = g_strdup ("https://example.invalid/skin.png");

  g_assert_true (comfy_accounts_add (store, account, &error));
  g_assert_no_error (error);

  /* The file is for nobody but the user: it holds the tokens. */
  check_permission (path, 0600);

  reopened = comfy_accounts_new_for_path (path);
  g_assert_cmpuint (comfy_accounts_count (reopened), ==, 1);

  found = comfy_accounts_get_active (reopened);
  g_assert_nonnull (found);
  g_assert_cmpint (found->type, ==, comfy_ACCOUNT_MICROSOFT);
  g_assert_cmpstr (found->name, ==, "Notch");
  g_assert_cmpstr (found->uuid, ==, "069a79f4-44e9-4726-a5be-fca90e38aaf5");
  g_assert_cmpstr (found->access_token, ==, "the-access-token");
  g_assert_cmpstr (found->refresh_token, ==, "the-refresh-token");
  g_assert_cmpint (found->expires_at, ==, 1800000000);
  g_assert_cmpstr (found->avatar_url, ==, "https://example.invalid/skin.png");

  /* A store that reads a file that is not there is a store with no
     accounts, not a store that fails to open. */
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  {
    g_autofree char *missing = temporary_path ("nothing-here.json");
    g_autoptr (ComfyAccounts) empty = comfy_accounts_new_for_path (missing);

    g_assert_cmpuint (comfy_accounts_count (empty), ==, 0);
    g_assert_null (comfy_accounts_get_active (empty));
  }

  /* Neither is a store whose file is nonsense. */
  {
    g_autofree char *broken = temporary_path ("broken.json");
    g_autoptr (ComfyAccounts) store_broken = NULL;

    g_assert_true (g_file_set_contents (broken, "{not a document", -1, NULL));

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                           "*accounts file*is corrupt; it was moved to*");
    store_broken = comfy_accounts_new_for_path (broken);
    g_test_assert_expected_messages ();
    g_assert_cmpuint (comfy_accounts_count (store_broken), ==, 0);
    g_assert_null (comfy_accounts_get_active (store_broken));
    {
      g_autofree char *quarantined = g_strconcat (broken, ".corrupt", NULL);

      g_assert_false (g_file_test (broken, G_FILE_TEST_EXISTS));
      g_assert_true (g_file_test (quarantined, G_FILE_TEST_IS_REGULAR));
    }
  }

  /* A type added by a newer or corrupted writer must not silently become
   * an offline account. */
  {
    g_autofree char *unknown = temporary_path ("unknown-type.json");
    g_autoptr (ComfyAccounts) store_unknown = NULL;
    const char *json =
      "{\"accounts\":[{\"type\":\"something-new\","
      "\"id\":\"id\",\"name\":\"Name\","
      "\"uuid\":\"uuid\"}],\"active\":\"id\"}";

    g_assert_true (g_file_set_contents (unknown, json, -1, NULL));
    store_unknown = comfy_accounts_new_for_path (unknown);
    g_assert_cmpuint (comfy_accounts_count (store_unknown), ==, 0);
    g_assert_null (comfy_accounts_get_active (store_unknown));
  }

  /* A field with the wrong JSON type is not a usable account either. */
  {
    g_autofree char *wrong_type = temporary_path ("wrong-field-type.json");
    g_autoptr (ComfyAccounts) store_wrong_type = NULL;
    const char *json =
      "{\"accounts\":[{\"type\":\"offline\",\"id\":42,"
      "\"name\":\"Name\",\"uuid\":\"uuid\"}]}";

    g_assert_true (g_file_set_contents (wrong_type, json, -1, NULL));
    store_wrong_type = comfy_accounts_new_for_path (wrong_type);
    g_assert_cmpuint (comfy_accounts_count (store_wrong_type), ==, 0);
  }

  /* What the file holds is the token in plain sight, which is why it is
     0600: a token is not a password, but it is not public either. */
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_assert_nonnull (strstr (text, "the-access-token"));

  g_print ("ok - accounts and their tokens survive a restart, 0600\n");
}

/* A view is told when the accounts or the active one are not what they
   were, which is what this counts. */
static void
on_changed (ComfyAccounts *store G_GNUC_UNUSED, gpointer user_data)
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
  g_autoptr (ComfyAccounts) store = NULL;
  g_autoptr (ComfyAccount) account = NULL;
  g_autoptr (ComfyAccount) other = NULL;
  g_autoptr (GError) error = NULL;
  guint changed = 0;

  store = comfy_accounts_new_for_path (path);

  g_signal_connect (store, "changed", G_CALLBACK (on_changed), &changed);

  account = microsoft_account (1800000000, "first-token");
  g_assert_true (comfy_accounts_add (store, account, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (changed, ==, 1);

  /* The same identity with a new token and a new expiry: one account,
     not two. */
  g_free (account->access_token);
  account->access_token = g_strdup ("second-token");
  account->expires_at = 1800003600;
  g_assert_true (comfy_accounts_update (store, account, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (comfy_accounts_count (store), ==, 1);
  g_assert_cmpstr (comfy_accounts_get_active (store)->access_token, ==,
                   "second-token");
  g_assert_cmpint (comfy_accounts_get_active (store)->expires_at, ==, 1800003600);
  g_assert_cmpuint (changed, ==, 2);

  /* Two offline accounts of one name are one account as well, because the
     name is what they are keyed by. */
  other = comfy_account_new_offline ("Flia", &error);
  g_assert_no_error (error);
  g_assert_true (comfy_accounts_add (store, other, &error));
  g_assert_true (comfy_accounts_add (store, other, &error));
  g_assert_cmpuint (comfy_accounts_count (store), ==, 2);
  g_assert_cmpstr (comfy_accounts_get_active_id (store), ==, "Flia");

  /* Picking the active account again is not a change: a view would do
     nothing about it and there is nothing to write. */
  changed = 0;
  g_assert_true (comfy_accounts_set_active (store, "Flia", &error));
  g_assert_no_error (error);
  g_assert_cmpuint (changed, ==, 0);

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
