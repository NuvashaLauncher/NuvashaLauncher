/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncher prepares and runs one instance, then owns its process state.
 * The preparation pipeline is asynchronous; the pure command-line helpers
 * remain usable without a JVM or network and are covered independently by
 * tests.
 *
 * The game must not inherit nuvashalauncher's GDK_BACKEND setting. Other environment
 * variables are left unchanged. */

#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-i18n.h"

#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-instance-components.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-line-reader.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-minecraft-versions.h"

#include <signal.h>
#include <string.h>

/* The three kinds of line the log holds. */
GQuark
nuvashalauncher_launch_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-launch-error");
}

/* ---------------------------------------------------------- *
 * The object
 * ---------------------------------------------------------- */

#include "nuvashalauncher-launcher-private.h"

G_DEFINE_FINAL_TYPE (nuvashalauncher, nuvashalauncher_launcher, G_TYPE_OBJECT)

guint nuvashalauncher_launcher_signals[nuvashalauncher_LAUNCHER_N_SIGNALS];


/* The account a launch plays as, or NULL when there is none. A launcher
 * made without a store is a launcher with no account, which is what a
 * test that drives one step at a time is. */
 nuvashalauncherAccount *
active_account (nuvashalauncher *self)
{
  if (self->accounts == NULL)
    return NULL;

  if (self->account_id_override != NULL)
    return nuvashalauncher_accounts_find (self->accounts, self->account_id_override);

  return nuvashalauncher_accounts_get_active (self->accounts);
}

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* The state and the detail it goes with go out together, and only when
 * one of them actually changed: a resolve reports progress many times a
 * second and a view has no use for the same line twice. */
void
set_state (nuvashalauncher  *self,
           nuvashalauncherLaunchState state,
           const char    *detail)
{
  gboolean changed;

  changed = self->state != state || g_strcmp0 (self->detail, detail) != 0;

  g_free (self->detail);
  self->detail = g_strdup (detail);
  self->state = state;

  if (changed)
    g_signal_emit (self, nuvashalauncher_launcher_signals[nuvashalauncher_LAUNCHER_SIGNAL_STATE], 0,
                   state, detail);
}

void
launcher_failed (nuvashalauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "%s", reason);
  set_state (self, nuvashalauncher_LAUNCH_FAILED, reason);
}

/* A launch that could not finish because the caller cancelled it, which
 * is what a stop does before the game starts, is not a failure. */
void
launch_finished_with_error (nuvashalauncher *self, GError *error)
{
  if (error != NULL &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      log_line (self, TAG_STATE,
                "the launch was cancelled before the game started");
      set_state (self, nuvashalauncher_LAUNCH_STOPPED, NULL);
      return;
    }

  launcher_failed (self, error != NULL ? error->message
                                       : "the launch failed");
}

/* ---------------------------------------------------------- *
 * The object itself
 * ---------------------------------------------------------- */

nuvashalauncherInstance *
nuvashalauncher_launcher_get_instance (nuvashalauncher *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), NULL);

  return self->instance;
}

nuvashalauncherLaunchState
nuvashalauncher_launcher_get_state (nuvashalauncher *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), nuvashalauncher_LAUNCH_STOPPED);

  return self->state;
}

const char *
nuvashalauncher_launcher_get_state_detail (nuvashalauncher *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), NULL);

  return self->detail;
}

const char *
nuvashalauncher_launcher_get_game_directory (nuvashalauncher *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), NULL);

  return self->game_directory;
}

char *
nuvashalauncher_launcher_get_log_directory (nuvashalauncher *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), NULL);

  return g_build_filename (self->game_directory, "logs", NULL);
}

char *
nuvashalauncher_launcher_get_crash_report_directory (nuvashalauncher *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), NULL);

  return g_build_filename (self->game_directory, "crash-reports", NULL);
}

nuvashalauncher *
nuvashalauncher_launcher_new (nuvashalauncherInstance *instance,
                  nuvashalauncherSettings *settings,
                  nuvashalauncherAccounts *accounts)
{
  nuvashalauncher *self;

  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (settings == NULL || nuvashalauncher_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (accounts == NULL || nuvashalauncher_IS_ACCOUNTS (accounts), NULL);

  self = g_object_new (nuvashalauncher_TYPE_LAUNCHER, NULL);
  self->instance = g_object_ref (instance);

  if (settings != NULL)
    self->settings = g_object_ref (settings);

  if (accounts != NULL)
    self->accounts = g_object_ref (accounts);

  g_object_get (instance, "directory", &self->game_directory, NULL);

  return self;
}

nuvashalauncherAccounts *
nuvashalauncher_launcher_get_accounts (nuvashalauncher *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), NULL);

  return self->accounts;
}

gboolean
nuvashalauncher_launcher_set_account (nuvashalauncher *self, const char *account_id,
                               GError **error)
{
  nuvashalauncherAccount *account;

  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  account = NULL;
  if (self->accounts != NULL)
    {
      account = nuvashalauncher_accounts_find (self->accounts, account_id);
      for (guint i = 0; account == NULL && i < nuvashalauncher_accounts_count (self->accounts); i++)
        {
          nuvashalauncherAccount *candidate =
            g_ptr_array_index (nuvashalauncher_accounts_list (self->accounts), i);
          if (g_strcmp0 (candidate->uuid, account_id) == 0 ||
              g_strcmp0 (candidate->name, account_id) == 0)
            account = candidate;
        }
    }
  if (account == NULL)
    {
      g_set_error (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_UNKNOWN,
                   "there is no account \"%s\"", account_id != NULL ? account_id : "");
      return FALSE;
    }

  g_free (self->account_id_override);
  self->account_id_override = g_strdup (account->id);
  return TRUE;
}

static void
nuvashalauncher_launcher_dispose (GObject *object)
{
  nuvashalauncher *self = nuvashalauncher_LAUNCHER (object);

  /* Downloads in flight stop when the owner lets go of the launcher.
     The game itself is not killed: the readers and the wait hold a
     reference for as long as it runs. */
  g_cancellable_cancel (self->preparation.cancellable);
  nuvashalauncher_task_scope_cancel (self->scope);

  G_OBJECT_CLASS (nuvashalauncher_launcher_parent_class)->dispose (object);
}

static void
nuvashalauncher_launcher_finalize (GObject *object)
{
  nuvashalauncher *self = nuvashalauncher_LAUNCHER (object);

  g_clear_object (&self->preparation.cancellable);
  nuvashalauncher_task_scope_unref (self->scope);
  self->scope = NULL;
  g_clear_object (&self->process.process);
  g_clear_pointer (&self->preparation.resolved, nuvashalauncher_resolved_version_free);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);
  g_clear_object (&self->accounts);
  g_clear_pointer (&self->account_id_override, g_free);
  if (self->lines != NULL)
    {
      g_queue_free_full (self->lines, g_free);
      self->lines = NULL;
    }
  g_free (self->detail);
  g_free (self->log_access_token);
  g_free (self->log_session);
  g_free (self->log_uuid);
  g_free (self->version);
  g_free (self->game_directory);
  g_free (self->preparation.java_executable);

  G_OBJECT_CLASS (nuvashalauncher_launcher_parent_class)->finalize (object);
}

static void
nuvashalauncher_launcher_class_init (nuvashalauncherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_launcher_dispose;
  object_class->finalize = nuvashalauncher_launcher_finalize;

  /* g_cclosure_marshal_generic marshals these: the state is an enum and
     the other arguments are strings, all of which it can copy. */
  nuvashalauncher_launcher_signals[nuvashalauncher_LAUNCHER_SIGNAL_STATE] =
    g_signal_new ("state", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 2, nuvashalauncher_TYPE_LAUNCH_STATE,
                  G_TYPE_STRING);

  nuvashalauncher_launcher_signals[nuvashalauncher_LAUNCHER_SIGNAL_OUTPUT] =
    g_signal_new ("output", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING,
                  G_TYPE_BOOLEAN);
}

static void
nuvashalauncher_launcher_init (nuvashalauncher *self)
{
  self->lines = g_queue_new ();
  self->scope = nuvashalauncher_task_scope_new (NULL, NULL, NULL, NULL);
  self->state = nuvashalauncher_LAUNCH_STOPPED;
}
