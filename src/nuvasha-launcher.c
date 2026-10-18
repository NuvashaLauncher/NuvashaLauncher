/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncher prepares and runs one instance, then owns its process state.
 * The preparation pipeline is asynchronous; the pure command-line helpers
 * remain usable without a JVM or network and are covered independently by
 * tests.
 *
 * The game must not inherit nuvasha's GDK_BACKEND setting. Other environment
 * variables are left unchanged. */

#include "nuvasha-launcher.h"
#include "nuvasha-i18n.h"

#include "nuvasha-auth-microsoft.h"
#include "nuvasha-instance-components.h"
#include "nuvasha-java.h"
#include "nuvasha-line-reader.h"
#include "nuvasha-loaders.h"
#include "nuvasha-minecraft-resolve.h"
#include "nuvasha-minecraft-versions.h"

#include <signal.h>
#include <string.h>

/* The three kinds of line the log holds. */
GQuark
nuvasha_launch_error_quark (void)
{
  return g_quark_from_static_string ("nuvasha-launch-error");
}

/* ---------------------------------------------------------- *
 * The object
 * ---------------------------------------------------------- */

#include "nuvasha-launcher-private.h"

G_DEFINE_FINAL_TYPE (NuvashaLauncher, nuvasha_launcher, G_TYPE_OBJECT)

guint nuvasha_launcher_signals[nuvasha_LAUNCHER_N_SIGNALS];


/* The account a launch plays as, or NULL when there is none. A launcher
 * made without a store is a launcher with no account, which is what a
 * test that drives one step at a time is. */
 NuvashaAccount *
active_account (NuvashaLauncher *self)
{
  if (self->accounts == NULL)
    return NULL;

  return nuvasha_accounts_get_active (self->accounts);
}

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* The state and the detail it goes with go out together, and only when
 * one of them actually changed: a resolve reports progress many times a
 * second and a view has no use for the same line twice. */
void
set_state (NuvashaLauncher  *self,
           NuvashaLaunchState state,
           const char    *detail)
{
  gboolean changed;

  changed = self->state != state || g_strcmp0 (self->detail, detail) != 0;

  g_free (self->detail);
  self->detail = g_strdup (detail);
  self->state = state;

  if (changed)
    g_signal_emit (self, nuvasha_launcher_signals[nuvasha_LAUNCHER_SIGNAL_STATE], 0,
                   state, detail);
}

void
launcher_failed (NuvashaLauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "%s", reason);
  set_state (self, nuvasha_LAUNCH_FAILED, reason);
}

/* A launch that could not finish because the caller cancelled it, which
 * is what a stop does before the game starts, is not a failure. */
void
launch_finished_with_error (NuvashaLauncher *self, GError *error)
{
  if (error != NULL &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      log_line (self, TAG_STATE,
                "the launch was cancelled before the game started");
      set_state (self, nuvasha_LAUNCH_STOPPED, NULL);
      return;
    }

  launcher_failed (self, error != NULL ? error->message
                                       : "the launch failed");
}

/* ---------------------------------------------------------- *
 * The object itself
 * ---------------------------------------------------------- */

NuvashaInstance *
nuvasha_launcher_get_instance (NuvashaLauncher *self)
{
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), NULL);

  return self->instance;
}

NuvashaLaunchState
nuvasha_launcher_get_state (NuvashaLauncher *self)
{
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), nuvasha_LAUNCH_STOPPED);

  return self->state;
}

const char *
nuvasha_launcher_get_state_detail (NuvashaLauncher *self)
{
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), NULL);

  return self->detail;
}

const char *
nuvasha_launcher_get_game_directory (NuvashaLauncher *self)
{
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), NULL);

  return self->game_directory;
}

char *
nuvasha_launcher_get_log_directory (NuvashaLauncher *self)
{
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), NULL);

  return g_build_filename (self->game_directory, "logs", NULL);
}

char *
nuvasha_launcher_get_crash_report_directory (NuvashaLauncher *self)
{
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), NULL);

  return g_build_filename (self->game_directory, "crash-reports", NULL);
}

NuvashaLauncher *
nuvasha_launcher_new (NuvashaInstance *instance,
                  NuvashaSettings *settings,
                  NuvashaAccounts *accounts)
{
  NuvashaLauncher *self;

  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (settings == NULL || nuvasha_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (accounts == NULL || nuvasha_IS_ACCOUNTS (accounts), NULL);

  self = g_object_new (nuvasha_TYPE_LAUNCHER, NULL);
  self->instance = g_object_ref (instance);

  if (settings != NULL)
    self->settings = g_object_ref (settings);

  if (accounts != NULL)
    self->accounts = g_object_ref (accounts);

  g_object_get (instance, "directory", &self->game_directory, NULL);

  return self;
}

NuvashaAccounts *
nuvasha_launcher_get_accounts (NuvashaLauncher *self)
{
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), NULL);

  return self->accounts;
}

static void
nuvasha_launcher_dispose (GObject *object)
{
  NuvashaLauncher *self = nuvasha_LAUNCHER (object);

  /* Downloads in flight stop when the owner lets go of the launcher.
     The game itself is not killed: the readers and the wait hold a
     reference for as long as it runs. */
  g_cancellable_cancel (self->preparation.cancellable);
  nuvasha_task_scope_cancel (self->scope);

  G_OBJECT_CLASS (nuvasha_launcher_parent_class)->dispose (object);
}

static void
nuvasha_launcher_finalize (GObject *object)
{
  NuvashaLauncher *self = nuvasha_LAUNCHER (object);

  g_clear_object (&self->preparation.cancellable);
  nuvasha_task_scope_unref (self->scope);
  self->scope = NULL;
  g_clear_object (&self->process.process);
  g_clear_pointer (&self->preparation.resolved, nuvasha_resolved_version_free);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);
  g_clear_object (&self->accounts);
  g_clear_pointer (&self->lines, g_ptr_array_unref);
  g_free (self->detail);
  g_free (self->log_access_token);
  g_free (self->log_session);
  g_free (self->log_uuid);
  g_free (self->version);
  g_free (self->game_directory);
  g_free (self->preparation.java_executable);

  G_OBJECT_CLASS (nuvasha_launcher_parent_class)->finalize (object);
}

static void
nuvasha_launcher_class_init (NuvashaLauncherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_launcher_dispose;
  object_class->finalize = nuvasha_launcher_finalize;

  /* g_cclosure_marshal_generic marshals these: the state is an enum and
     the other arguments are strings, all of which it can copy. */
  nuvasha_launcher_signals[nuvasha_LAUNCHER_SIGNAL_STATE] =
    g_signal_new ("state", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 2, nuvasha_TYPE_LAUNCH_STATE,
                  G_TYPE_STRING);

  nuvasha_launcher_signals[nuvasha_LAUNCHER_SIGNAL_OUTPUT] =
    g_signal_new ("output", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING,
                  G_TYPE_BOOLEAN);
}

static void
nuvasha_launcher_init (NuvashaLauncher *self)
{
  self->lines = g_ptr_array_new_with_free_func (g_free);
  self->scope = nuvasha_task_scope_new (NULL, NULL, NULL, NULL);
  self->state = nuvasha_LAUNCH_STOPPED;
}
