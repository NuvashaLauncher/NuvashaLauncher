/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-launcher-private.h"
#include "aml-auth-microsoft.h"
#include "aml-instance-components.h"
#include "aml-i18n.h"

/* Internal launch start/stop control workflow. */

/* ---------------------------------------------------------- *
 * Starting and stopping
 * ---------------------------------------------------------- */

/* The first request of a launch: the version manifest, and from there
 * everything a launch needs before the game can start. It is a function
 * of its own because an account that had to be signed in again carries
 * on from here with the token it just got. */
static void
start_preparing (AmlLauncher *self)
{
  log_line (self, TAG_STATE, "preparing Minecraft %s",
            self->version != NULL ? self->version : "this instance");

  {
    AmlTaskScope *fetching =
      aml_version_manifest_fetch_async (AML_VERSION_MANIFEST_URL,
                                        on_manifest_ready, g_object_ref (self));

    /* One read of a launch: it stops with the launch, and releases itself
       when the launch has what it needs. */
    aml_task_scope_follow_cancellable (fetching, self->preparation.cancellable);
    aml_task_scope_unref_when_done (fetching);
  }
}

/* A sign-in that has to happen again, said as a state of its own: nothing
 * is running, nothing is broken, and the account is what the user has to
 * take care of. A token that cannot be traded for a new one is dropped
 * from the store, so that the account says it needs a sign-in wherever it
 * is shown instead of pretending to be ready. */
void
launcher_needs_sign_in (AmlLauncher *self, const char *reason)
{
  AmlAccount *account = active_account (self);
  g_autofree char *detail = NULL;

  if (account != NULL)
    {
      AmlAccount *without = aml_account_copy (account);
      g_clear_pointer (&without->access_token, g_free);
      g_clear_pointer (&without->refresh_token, g_free);
      without->expires_at = 0;

      /* A store that cannot be written is not a launch that is broken:
         the state still says what the user has to do. */
      aml_accounts_update (self->accounts, without, NULL);
      aml_account_free (without);
    }

  detail = g_strdup_printf ("%s: open the users menu and sign in as \"%s\" "
                            "again",
                            reason,
                            account != NULL && account->name != NULL
                              ? account->name
                              : "the account");

  log_line (self, TAG_STATE, "%s", detail);
  set_state (self, AML_LAUNCH_NEEDS_SIGN_IN, detail);
}

/* A new token for an account whose old one has run out. */
static void
on_account_refreshed (GObject      *source G_GNUC_UNUSED,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr (AmlLauncher) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlAccount) account = NULL;

  account = aml_auth_refresh_finish (result, &error);

  if (self->process.stop_requested)
    return;

  if (account == NULL)
    {
      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          log_line (self, TAG_STATE,
                    "the launch was cancelled before the game started");
          set_state (self, AML_LAUNCH_STOPPED, NULL);
          return;
        }

      launcher_needs_sign_in (
          self, error != NULL ? error->message : "the account could not be "
                                                     "signed in again");
      return;
    }

  /* The refreshed token is what the rest of the launch runs with, and it
     is stored so that the next launch does not have to ask again. */
  if (!aml_accounts_update (self->accounts, account, &error))
    log_line (self, TAG_STATE,
              "the refreshed token could not be stored: %s", error->message);

  start_preparing (self);
}

/* The account a launch runs as. Nobody signed in is not a reason to
 * refuse: the game starts offline as Player. An account whose token has
 * run out is signed in again before anything is downloaded, because a
 * game that is going to need a token later can fail now in words
 * instead. */
static gboolean
account_for_launch (AmlLauncher *self)
{
  AmlAccount *account = active_account (self);

  if (account == NULL)
    return TRUE;

  if (account->type == AML_ACCOUNT_OFFLINE)
    return TRUE;

  if (aml_account_token_is_valid (account, g_get_real_time () / 1000000))
    return TRUE;

  log_line (self, TAG_STATE, "signing \"%s\" in again before launching",
            account->name != NULL ? account->name : "the account");

  aml_task_scope_release_when_done (
    aml_auth_refresh_async (account, on_account_refreshed, g_object_ref (self)),
    self->preparation.cancellable);

  return FALSE;
}

void
aml_launcher_start (AmlLauncher *self)
{
  /* Not g_autofree: the instance hands the string over and it becomes the
     version of this launch, which outlives this function. */
  char *version = NULL;

  g_return_if_fail (AML_IS_LAUNCHER (self));

  /* The state is the whole answer to "is this instance already being
     launched": a second launch is refused instead of racing the first. */
  if (aml_launch_state_is_busy (self->state))
    {
      log_line (self, TAG_STATE,
                "Minecraft %s is already %s, nothing was started",
                self->version != NULL ? self->version : "",
                aml_launch_state_to_string (self->state));
      return;
    }

  version = aml_instance_component_get_version (
    self->instance, AML_COMPONENT_MINECRAFT);

  g_free (self->version);
  self->version = version;

  g_clear_object (&self->preparation.cancellable);
  self->preparation.cancellable = g_cancellable_new ();
  self->process.stop_requested = FALSE;
  self->process.last_percent = 0;

  /* The Java of the launch before this one says nothing about this one:
     a loader that has to be installed first chooses its own. */
  g_clear_pointer (&self->preparation.java_executable, g_free);
  self->preparation.java_major_version = 0;

  set_state (self, AML_LAUNCH_PREPARING, self->version);

  /* Without an account there is nobody to play as, and nothing to
     download for. An account that has to be signed in again is signed in
     again first, and carries on from here when it has one. */
  if (!account_for_launch (self))
    return;

  start_preparing (self);
}

void
aml_launcher_stop (AmlLauncher *self)
{
  g_return_if_fail (AML_IS_LAUNCHER (self));

  if (!aml_launch_state_is_busy (self->state))
    return;

  self->process.stop_requested = TRUE;

  /* Nothing is running yet: what has to stop is the resolve and the
     Java search behind it. */
  if (self->process.process == NULL)
    {
      log_line (self, TAG_STATE, "stopping before the game starts");
      g_cancellable_cancel (self->preparation.cancellable);
      set_state (self, AML_LAUNCH_STOPPED, NULL);
      return;
    }

  set_state (self, AML_LAUNCH_STOPPING, "asking the game to stop");
  log_line (self, TAG_STATE, "asking the game to stop");

  /* Minecraft has its own way of ending; SIGTERM is what every launcher
     sends first, and a game that hangs is stopped for good after the
     timeout. */
  g_subprocess_send_signal (self->process.process, SIGTERM);

  if (self->process.stop_timeout == 0)
    {
      self->process.stop_timeout =
        g_timeout_add_seconds (AML_LAUNCH_STOP_TIMEOUT, on_stop_timeout, self);
      aml_task_scope_track_source (self->scope, self->process.stop_timeout);
    }
}
