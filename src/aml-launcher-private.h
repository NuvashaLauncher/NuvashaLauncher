/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_LAUNCHER_PRIVATE_H
#define AML_LAUNCHER_PRIVATE_H

#include "aml-launcher.h"
#include "aml-java.h"

#define TAG_STATE "state"
#define TAG_STDOUT "stdout"
#define TAG_STDERR "stderr"

struct _AmlLauncher
{
  GObject parent_instance;

  AmlInstance *instance;
  AmlSettings *settings;
  char *version;
  char *game_directory;

  AmlLaunchState state;
  char *detail; /* what goes with the state, NULL when there is nothing */

  /* The log: the newest line last, trimmed to what the settings ask
     for. */
  GPtrArray *lines;

  /* What a launch needs while it is on its way. */
  GCancellable *cancellable;
  AmlResolvedVersion *resolved;
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
  AmlAccounts *accounts;

  /* The Java this launch chose. A loader that has to be installed first
   * needs one before the game is resolved, so the choice is made once and
   * kept. */
  char *java_executable;
  int java_major_version;

};

extern guint aml_launcher_signals[AML_LAUNCHER_N_SIGNALS];

void log_line (AmlLauncher *self, const char *tag, const char *format, ...)
  G_GNUC_PRINTF (3, 4);
void set_state (AmlLauncher *self, AmlLaunchState state,
                const char *detail);
void launcher_failed (AmlLauncher *self, const char *reason);
gboolean on_stop_timeout (gpointer user_data);
AmlAccount *active_account (AmlLauncher *self);
void on_manifest_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data);
void launcher_needs_sign_in (AmlLauncher *self, const char *reason);
void launch_finished_with_error (AmlLauncher *self, GError *error);
const AmlJava *choose_java (AmlLauncher *self, GPtrArray *javas, int required,
                            char **instance_java);
void remember_java (AmlLauncher *self, const AmlJava *java);
void on_resolved (GObject *source, GAsyncResult *result, gpointer user_data);
void on_resolve_progress (const AmlResolveProgress *progress,
                          gpointer user_data);

#endif /* AML_LAUNCHER_PRIVATE_H */
