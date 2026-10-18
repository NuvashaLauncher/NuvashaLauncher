/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_LAUNCHER_PRIVATE_H
#define nuvasha_LAUNCHER_PRIVATE_H

#include "nuvasha-launcher.h"
#include "nuvasha-java.h"
#include "nuvasha-task.h"

#define TAG_STATE "state"
#define TAG_STDOUT "stdout"
#define TAG_STDERR "stderr"

struct _NuvashaLauncher
{
  GObject parent_instance;

  NuvashaInstance *instance;
  NuvashaSettings *settings;
  char *version;
  char *game_directory;

  NuvashaLaunchState state;
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
    NuvashaResolvedVersion *resolved;
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
  NuvashaAccounts *accounts;
  NuvashaTaskScope *scope;

};

extern guint nuvasha_launcher_signals[nuvasha_LAUNCHER_N_SIGNALS];

void log_line (NuvashaLauncher *self, const char *tag, const char *format, ...)
  G_GNUC_PRINTF (3, 4);
void set_state (NuvashaLauncher *self, NuvashaLaunchState state,
                const char *detail);
void launcher_failed (NuvashaLauncher *self, const char *reason);
gboolean on_stop_timeout (gpointer user_data);
void finish_launch (NuvashaLauncher *self);
NuvashaAccount *active_account (NuvashaLauncher *self);
void on_manifest_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data);
void launcher_needs_sign_in (NuvashaLauncher *self, const char *reason);
void launch_finished_with_error (NuvashaLauncher *self, GError *error);
const NuvashaJava *choose_java (NuvashaLauncher *self, GPtrArray *javas, int required,
                            char **instance_java);
void remember_java (NuvashaLauncher *self, const NuvashaJava *java);
void on_resolved (GObject *source, GAsyncResult *result, gpointer user_data);
void on_resolve_progress (const NuvashaResolveProgress *progress,
                          gpointer user_data);

#endif /* nuvasha_LAUNCHER_PRIVATE_H */
