/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_LAUNCHER_PRIVATE_H
#define leppoisa_LAUNCHER_PRIVATE_H

#include "leppoisa-launcher.h"
#include "leppoisa-java.h"
#include "leppoisa-task.h"

#define TAG_STATE "state"
#define TAG_STDOUT "stdout"
#define TAG_STDERR "stderr"

struct _LeppoisaLauncher
{
  GObject parent_instance;

  LeppoisaInstance *instance;
  LeppoisaSettings *settings;
  char *version;
  char *game_directory;

  LeppoisaLaunchState state;
  char *detail; /* what goes with the state, NULL when there is nothing */

  /* Per-launch values that must not appear in game output or launcher logs. */
  char *log_access_token;
  char *log_session;
  char *log_uuid;

  /* The log: the newest line last, trimmed to what the settings ask
     for. */
  GPtrArray *lines;

  /* What preparation has selected while a launch is on its way. */
  struct {
    GCancellable *cancellable;
    LeppoisaResolvedVersion *resolved;
    char *java_executable;
    int java_major_version;
  } preparation;
  struct {
    GSubprocess *process;
    gboolean exited;
    gboolean exited_normally;
    gboolean terminated_by_signal;
    int exit_status;
    int term_signal;
    gint64 started_at; /* unix seconds, for crash reports */
    guint readers;
    guint stop_timeout;
    guint last_percent;
    gboolean stop_requested;
  } process;

  /* The accounts a launch plays as. The active one of them is read when
     the command line is built, so a token that was refreshed a moment
     earlier is the one the game is run with. */
  LeppoisaAccounts *accounts;
  LeppoisaTaskScope *scope;

};

extern guint leppoisa_launcher_signals[leppoisa_LAUNCHER_N_SIGNALS];

void log_line (LeppoisaLauncher *self, const char *tag, const char *format, ...)
  G_GNUC_PRINTF (3, 4);
void set_state (LeppoisaLauncher *self, LeppoisaLaunchState state,
                const char *detail);
void launcher_failed (LeppoisaLauncher *self, const char *reason);
gboolean on_stop_timeout (gpointer user_data);
void finish_launch (LeppoisaLauncher *self);
LeppoisaAccount *active_account (LeppoisaLauncher *self);
void on_manifest_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data);
void launcher_needs_sign_in (LeppoisaLauncher *self, const char *reason);
void launch_finished_with_error (LeppoisaLauncher *self, GError *error);
const LeppoisaJava *choose_java (LeppoisaLauncher *self, GPtrArray *javas, int required,
                            char **instance_java);
void remember_java (LeppoisaLauncher *self, const LeppoisaJava *java);
void on_resolved (GObject *source, GAsyncResult *result, gpointer user_data);
void on_resolve_progress (const LeppoisaResolveProgress *progress,
                          gpointer user_data);

#endif /* leppoisa_LAUNCHER_PRIVATE_H */
