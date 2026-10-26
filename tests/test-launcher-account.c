/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for the account a launch runs as: playing offline when there is
 * none, and what happens when the one there is cannot be signed in
 * again.
 *
 * Nothing here waits for a service and nothing needs a display: the
 * sign-in refusal happens before anything is asked for, because an
 * account with no refresh token has nothing to trade, and every launch
 * that gets past its gate is stopped again at once. The one request that
 * does go out is the manifest of such a launch. */

#include "nuvasha-launcher.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static char *root;

/* The refresh is asked for and answered through the main loop, which is
   where every answer of a launch comes from: this runs the loop until the
   state the test is waiting for arrives, and gives up rather than hanging
   if it never does. */
typedef struct
{
  GMainLoop *loop;
  guint watchdog;
  gboolean timed_out;
  NuvashaLaunchState wanted;
} Watch;

static gboolean
watchdog_fired (gpointer user_data)
{
  Watch *watch = user_data;

  watch->timed_out = TRUE;
  g_main_loop_quit (watch->loop);

  return G_SOURCE_REMOVE;
}

static void
on_state (NuvashaLauncher *launcher G_GNUC_UNUSED, NuvashaLaunchState state,
          const char *detail G_GNUC_UNUSED, gpointer user_data)
{
  Watch *watch = user_data;

  if (state == watch->wanted || !nuvasha_launch_state_is_busy (state))
    g_main_loop_quit (watch->loop);
}

static void
watch_until (Watch *watch, NuvashaLauncher *launcher, NuvashaLaunchState wanted)
{
  watch->wanted = wanted;

  /* The answer to "is this account good" is synchronous, so the state is
     sometimes already the one that is waited for before the loop runs. */
  if (nuvasha_launcher_get_state (launcher) == wanted ||
      !nuvasha_launch_state_is_busy (nuvasha_launcher_get_state (launcher)))
    return;

  watch->loop = g_main_loop_new (NULL, FALSE);
  watch->watchdog = g_timeout_add_seconds (30, watchdog_fired, watch);
  g_signal_connect (launcher, "state", G_CALLBACK (on_state), watch);

  g_main_loop_run (watch->loop);

  if (!watch->timed_out)
    g_source_remove (watch->watchdog);

  g_assert_false (watch->timed_out);

  g_signal_handlers_disconnect_by_data (launcher, watch);
  g_main_loop_unref (watch->loop);
  watch->loop = NULL;
}

/* ---------------------------------------------------------- *
 * A place to work and something to work on
 * ---------------------------------------------------------- */

static NuvashaInstance *
new_instance (void)
{
  g_autofree char *directory = g_build_filename (root, "instance", NULL);
  g_autoptr (GError) error = NULL;
  NuvashaInstance *instance = NULL;

  if (g_mkdir_with_parents (directory, 0755) != 0)
    g_error ("the instance directory \"%s\" cannot be made", directory);

  instance = nuvasha_instance_new (NULL, "one", directory, "1.20.1",
                               nuvasha_LOADER_VANILLA, &error);

  if (instance == NULL)
    g_error ("the test instance cannot be made: %s", error->message);

  return instance;
}

static NuvashaAccounts *
new_store (const char *name)
{
  g_autofree char *path = g_build_filename (root, name, NULL);

  return nuvasha_accounts_new_for_path (path);
}

static void
assert_log_has (NuvashaLauncher *launcher, const char *needle)
{
  g_autofree char *log = nuvasha_launcher_get_log_text (launcher);

  if (strstr (log, needle) == NULL)
    g_error ("the log has no \"%s\":\n%s", needle, log);
}

static void
assert_log_lacks (NuvashaLauncher *launcher, const char *needle)
{
  g_autofree char *log = nuvasha_launcher_get_log_text (launcher);

  if (strstr (log, needle) != NULL)
    g_error ("the log has \"%s\" where it should not:\n%s", needle, log);
}

/* A Microsoft account as it is after a sign-in: a profile, a token that
 * is good for another hour and a refresh token to trade for the next
 * one. */
static NuvashaAccount *
microsoft_account (gboolean with_refresh_token)
{
  NuvashaAccount *account = g_new0 (NuvashaAccount, 1);

  account->type = nuvasha_ACCOUNT_MICROSOFT;
  account->id = g_strdup ("069a79f444e94726a5befca90e38aaf5");
  account->name = g_strdup ("Notch");
  account->uuid = g_strdup ("069a79f4-44e9-4726-a5be-fca90e38aaf5");
  account->access_token = g_strdup ("a-token-that-is-good-for-an-hour");
  account->expires_at = g_get_real_time () / 1000000 + 3600;

  if (with_refresh_token)
    account->refresh_token = g_strdup ("the-refresh-token");

  return account;
}

/* ---------------------------------------------------------- *
 * No account at all
 * ---------------------------------------------------------- */

/* A launch with nobody to play as still starts: the game plays offline
   as Player, the name a fresh Minecraft falls back to. The launch is
   stopped again at once, before the manifest request it just made. */
static void
test_no_account_plays_offline (void)
{
  g_autoptr (NuvashaInstance) instance = new_instance ();
  g_autoptr (NuvashaAccounts) accounts = new_store ("no-accounts.json");
  g_autoptr (NuvashaLauncher) launcher =
    nuvasha_launcher_new (instance, NULL, accounts);

  g_assert_cmpint (nuvasha_launcher_get_state (launcher), ==, nuvasha_LAUNCH_STOPPED);

  nuvasha_launcher_start (launcher);

  /* Preparing means the gate let it through: a launch that stopped at an
     account would be Failed before this. */
  g_assert_cmpint (nuvasha_launcher_get_state (launcher), ==, nuvasha_LAUNCH_PREPARING);
  assert_log_lacks (launcher, "no account to play as");
  assert_log_has (launcher, "preparing Minecraft 1.20.1");

  nuvasha_launcher_stop (launcher);

  /* A launcher without any accounts at all behaves the same rather than
     refusing to read a store that is not there. */
  {
    g_autoptr (NuvashaLauncher) bare = nuvasha_launcher_new (instance, NULL, NULL);

    g_assert_null (nuvasha_launcher_get_accounts (bare));

    nuvasha_launcher_start (bare);
    g_assert_cmpint (nuvasha_launcher_get_state (bare), ==, nuvasha_LAUNCH_PREPARING);
    assert_log_lacks (bare, "no account to play as");

    nuvasha_launcher_stop (bare);
  }

  g_print ("ok - a launch with no account plays offline as Player\n");
}

/* ---------------------------------------------------------- *
 * An account that has to sign in again
 * ---------------------------------------------------------- */

/* An account whose token has run out and cannot be traded for a new one
   is a state of its own: nothing is running, nothing is broken, and what
   the user has to do is written down where a launch shows it. */
static void
test_a_dead_token_asks_for_a_sign_in (void)
{
  g_autoptr (NuvashaInstance) instance = new_instance ();
  g_autoptr (NuvashaAccounts) accounts = new_store ("dead-token.json");
  g_autoptr (NuvashaLauncher) launcher =
    nuvasha_launcher_new (instance, NULL, accounts);
  g_autoptr (NuvashaAccount) account = NULL;
  g_autoptr (GError) error = NULL;
  NuvashaAccount *active = NULL;
  Watch watch = { 0 };

  /* An account of a family whose Microsoft sign-in has gone: the token
     is there and already past its hour, and there is no refresh token
     behind it, which is what happens when a sign-in is revoked. */
  account = microsoft_account (FALSE);
  account->expires_at = g_get_real_time () / 1000000 - 10;

  g_assert_true (nuvasha_accounts_add (accounts, account, &error));
  g_assert_no_error (error);

  nuvasha_launcher_start (launcher);

  watch_until (&watch, launcher, nuvasha_LAUNCH_NEEDS_SIGN_IN);

  g_assert_cmpint (nuvasha_launcher_get_state (launcher), ==,
                   nuvasha_LAUNCH_NEEDS_SIGN_IN);
  assert_log_has (launcher, "sign in again");
  assert_log_has (launcher, "Notch");

  /* What goes with the state is the sentence the user is shown. */
  {
    const char *detail = nuvasha_launcher_get_state_detail (launcher);

    g_assert_nonnull (detail);
    g_assert_nonnull (strstr (detail, "sign in"));
    g_assert_nonnull (strstr (detail, "Notch"));
  }

  /* A launch that needs a sign-in is not busy: nothing is running, and
     the user can start it again as soon as they have signed in. */
  g_assert_false (nuvasha_launch_state_is_busy (nuvasha_launcher_get_state (launcher)));

  /* The token that could not be traded is dropped from the store, so that
     the account says everywhere it is shown that it needs a sign-in
     rather than pretending to be ready. */
  active = nuvasha_accounts_get_active (accounts);
  g_assert_nonnull (active);
  g_assert_null (active->access_token);
  g_assert_null (active->refresh_token);
  g_assert_cmpint (active->expires_at, ==, 0);

  /* The name and the UUID of the account are what it is: a token that is
     gone does not make the account a different one. */
  g_assert_cmpstr (active->name, ==, "Notch");
  g_assert_cmpstr (active->uuid, ==, "069a79f4-44e9-4726-a5be-fca90e38aaf5");

  /* And the token is not in the log: it never was, and now it is gone
     from the account as well. */
  assert_log_lacks (launcher, "a-token-that-is-good-for-an-hour");

  g_print ("ok - an account that cannot be refreshed needs a sign-in\n");
}

/* An account that is ready goes past the gate: the log says what the
   launch is preparing and the state is Preparing rather than Failed.
   The manifest request that follows is stopped again at once: what is
   being tested here is the gate, not the manifest. */
static void
test_a_ready_account_gets_past_the_gate (void)
{
  g_autoptr (NuvashaInstance) instance = new_instance ();
  g_autoptr (NuvashaAccounts) accounts = new_store ("ready.json");
  g_autoptr (NuvashaLauncher) launcher =
    nuvasha_launcher_new (instance, NULL, accounts);
  g_autoptr (NuvashaAccount) account = microsoft_account (TRUE);
  g_autoptr (GError) error = NULL;

  g_assert_true (nuvasha_accounts_add (accounts, account, &error));
  g_assert_no_error (error);

  nuvasha_launcher_start (launcher);

  /* Preparing means the account was good: a launch that stops at the
     account never gets there. */
  g_assert_cmpint (nuvasha_launcher_get_state (launcher), ==,
                   nuvasha_LAUNCH_PREPARING);
  assert_log_lacks (launcher, "no account to play as");
  assert_log_has (launcher, "preparing Minecraft 1.20.1");

  /* The launch is stopped again so that nothing is left fetching. */
  nuvasha_launcher_stop (launcher);

  g_print ("ok - a ready account is what a launch goes out with\n");
}

static void
test_launch_can_use_another_account (void)
{
  g_autoptr (NuvashaInstance) instance = new_instance ();
  g_autoptr (NuvashaAccounts) accounts = new_store ("launch-account.json");
  g_autoptr (NuvashaLauncher) launcher =
    nuvasha_launcher_new (instance, NULL, accounts);
  g_autoptr (NuvashaAccount) first = NULL;
  g_autoptr (NuvashaAccount) second = NULL;
  g_autoptr (GError) error = NULL;

  first = nuvasha_account_new_offline ("Alex", &error);
  g_assert_no_error (error);
  second = nuvasha_account_new_offline ("Steve", &error);
  g_assert_no_error (error);
  g_assert_true (nuvasha_accounts_add (accounts, first, &error));
  g_assert_true (nuvasha_accounts_add (accounts, second, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (nuvasha_accounts_get_active (accounts)->name, ==, "Steve");

  g_assert_true (nuvasha_launcher_set_account (launcher, first->uuid, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (nuvasha_accounts_get_active (accounts)->name, ==, "Steve");

  g_print ("ok - a launch can use an account without changing the active one\n");
}

/* The name of the state is what a view shows, so it is part of what the
   launcher promises. */
static void
test_the_state_has_a_name (void)
{
  g_assert_cmpstr (nuvasha_launch_state_to_string (nuvasha_LAUNCH_NEEDS_SIGN_IN), ==,
                   "Needs sign-in");
  g_assert_cmpstr (nuvasha_launch_state_to_string (nuvasha_LAUNCH_PREPARING), ==,
                   "Preparing");
  g_assert_cmpstr (nuvasha_launch_state_to_string (nuvasha_LAUNCH_FAILED), ==,
                   "Failed");
  g_assert_false (nuvasha_launch_state_is_busy (nuvasha_LAUNCH_NEEDS_SIGN_IN));
  g_assert_true (nuvasha_launch_state_is_busy (nuvasha_LAUNCH_PREPARING));

  g_print ("ok - the sign-in state is a state with a name\n");
}

int
main (void)
{
  g_autofree char *directory = NULL;

  g_print ("# Launcher account tests\n");

  directory = g_dir_make_tmp ("nuvasha-launcher-account-XXXXXX", NULL);

  if (directory == NULL)
    g_error ("a temporary directory cannot be made");

  root = directory;

  test_no_account_plays_offline ();
  test_a_dead_token_asks_for_a_sign_in ();
  test_a_ready_account_gets_past_the_gate ();
  test_launch_can_use_another_account ();
  test_the_state_has_a_name ();

  return EXIT_SUCCESS;
}
