/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_LAUNCHER_PRIVATE_H
#define nuvashalauncher_LAUNCHER_PRIVATE_H

#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-task.h"

#define TAG_STATE "state"
#define TAG_STDOUT "stdout"
#define TAG_STDERR "stderr"

struct _NuvashaLauncher
{
  GObject parent_instance;

  NuvashaLauncherInstance *instance;
  NuvashaLauncherSettings *settings;
  char *version;
  char *game_directory;

  NuvashaLauncherLaunchState state;
  char *detail; /* optional state detail */

  /* Per-launch values that must not appear in game output or launcher logs. */
  char *log_access_token;
  char *log_session;
  char *log_uuid;

  /* Log lines, newest last and bounded by settings. */
  GQueue *lines;

  /* Values selected during launch preparation. */
  struct {
    GCancellable *cancellable;
    NuvashaLauncherResolvedVersion *resolved;
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
    gint64 started_at; /* unix microseconds, for crash reports */
    guint readers;
    guint stop_timeout;
    guint last_percent;
    gboolean stop_requested;
  } process;

  /* Accounts used by the launch; the active account is read when building
   * the command line. */
  NuvashaLauncherAccounts *accounts;
  char *account_id_override;
  NuvashaLauncherTaskScope *scope;

};

extern guint nuvashalauncher_launcher_signals[nuvashalauncher_LAUNCHER_N_SIGNALS];

void log_line (NuvashaLauncher *self, const char *tag, const char *format, ...)
  G_GNUC_PRINTF (3, 4);
void set_state (NuvashaLauncher *self, NuvashaLauncherLaunchState state,
                const char *detail);
void launcher_failed (NuvashaLauncher *self, const char *reason);
gboolean on_stop_timeout (gpointer user_data);
void finish_launch (NuvashaLauncher *self);
NuvashaLauncherAccount *active_account (NuvashaLauncher *self);
void on_manifest_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data);
void launcher_needs_sign_in (NuvashaLauncher *self, const char *reason);
void launch_finished_with_error (NuvashaLauncher *self, GError *error);
void launcher_auth_failed (NuvashaLauncher *self, const GError *error);
const NuvashaLauncherJava *choose_java (NuvashaLauncher *self, GPtrArray *javas, int required,
                            char **instance_java);
void remember_java (NuvashaLauncher *self, const NuvashaLauncherJava *java);
void on_resolved (GObject *source, GAsyncResult *result, gpointer user_data);
void on_resolve_progress (const NuvashaLauncherResolveProgress *progress,
                          gpointer user_data);
void nuvashalauncher_launcher_signal_process_group (NuvashaLauncher *self,
                                                    int signal_number);

#endif /* nuvashalauncher_LAUNCHER_PRIVATE_H */
