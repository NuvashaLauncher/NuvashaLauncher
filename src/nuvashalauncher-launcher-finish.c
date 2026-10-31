/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-launcher-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-i18n.h"

/* ---------------------------------------------------------- *
 * Where the game ends
 * ---------------------------------------------------------- */

static void
game_crashed (nuvashalauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "the game crashed: %s", reason);
  set_state (self, nuvashalauncher_LAUNCH_CRASHED, reason);
}

static void finish_after_natives_removed (GObject *source, GAsyncResult *result,
                                          gpointer user_data);
static void finish_launch_after_natives (nuvashalauncher *self);

/* Nothing else can make a game that ended with zero a crash except a
 * report it wrote while it was dying, so that is what is asked for. A
 * report of an earlier run does not count: the launch time is what the
 * modification time is compared against. */
static void
crash_reports_listed (GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
  nuvashalauncher *self = nuvashalauncher_LAUNCHER (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileEnumerator) files = NULL;
  g_autoptr (GFileInfo) info = NULL;
  gboolean crashed = FALSE;

  files = g_file_enumerate_children_finish (G_FILE (source), result, &error);

  if (files != NULL)
    {
      while ((info = g_file_enumerator_next_file (files, NULL, NULL)) != NULL)
        {
          gboolean written_now = FALSE;

          if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
            {
              guint64 modified_seconds = g_file_info_get_attribute_uint64 (
                info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
              gint64 started_seconds = self->process.started_at / G_USEC_PER_SEC;

              /* Crash-report mtimes are not consistently available with
               * microsecond precision (notably on some ARM filesystems).
               * The launch boundary is deliberately second-granularity:
               * accepting any report from the launch second avoids treating
               * a report written after spawn as an old report merely because
               * its timestamp was rounded down. */
              written_now = modified_seconds >= (guint64) started_seconds;
            }

          /* Every report is looked at, and what is left of the
             directory goes with the loop instead of piling up. */
          g_clear_object (&info);

          if (written_now)
            {
              crashed = TRUE;
              break;
            }
        }
    }

  if (crashed)
    game_crashed (self, _("A new crash report was created. Open the logs for details."));
  else
    set_state (self, nuvashalauncher_LAUNCH_STOPPED, NULL);

  g_object_unref (self);
}

static void
report_how_it_ended (nuvashalauncher *self)
{
  g_autofree char *reason = NULL;

  if (self->process.terminated_by_signal)
    reason = g_strdup_printf (_("the game was stopped by signal %d"),
                             self->process.term_signal);
  else
    reason = g_strdup_printf (_("the game exited with code %d"),
                             self->process.exit_status);

  game_crashed (self, reason);
}

/* The process is gone and both pipes have been read to the end, so the
 * launch is over and the state can be the final one. */
void
finish_launch (nuvashalauncher *self)
{
  /* Natives are launch artifacts, not instance data. They are extracted from
   * the cached jars again on the next launch, so removing this per-instance
   * directory prevents a game or mod from leaving native code behind. */
  {
    g_autofree char *natives =
      g_build_filename (self->game_directory, "natives", NULL);
    nuvashalauncherTaskScope *cleanup =
      nuvashalauncher_fs_remove_tree_async (natives,
                                            finish_after_natives_removed,
                                            g_object_ref (self));

    /* The callback owns the launcher reference until cleanup completes. */
    nuvashalauncher_task_scope_unref_when_done (cleanup);
  }
}

static void
finish_launch_after_natives (nuvashalauncher *self)
{

  if (self->process.stop_timeout != 0)
    {
      /* Through the scope, which is where this source is registered. */
      nuvashalauncher_task_scope_remove_source (self->scope, self->process.stop_timeout);
      self->process.stop_timeout = 0;
    }

  /* A game that was asked to stop and did, however it did, has not
     crashed: the force stop reports itself in the log instead. */
  if (self->process.stop_requested)
    {
      log_line (self, TAG_STATE, "the game stopped");
      set_state (self, nuvashalauncher_LAUNCH_STOPPED, NULL);
      return;
    }

  if (!self->process.exited_normally || self->process.exit_status != 0)
    {
      report_how_it_ended (self);
      return;
    }

  {
    g_autofree char *directory = nuvashalauncher_launcher_get_crash_report_directory (self);
    g_autoptr (GFile) reports = g_file_new_for_path (directory);

    if (!g_file_query_exists (reports, NULL))
      {
        set_state (self, nuvashalauncher_LAUNCH_STOPPED, NULL);
        return;
      }

    g_file_enumerate_children_async (
      reports,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
      G_FILE_ATTRIBUTE_TIME_MODIFIED,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, NULL,
      crash_reports_listed, g_object_ref (self));
  }
}

static void
finish_after_natives_removed (GObject *source G_GNUC_UNUSED,
                              GAsyncResult *result,
                              gpointer user_data)
{
  nuvashalauncher *self = nuvashalauncher_LAUNCHER (user_data);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_fs_remove_tree_finish (result, &error) && error != NULL)
    log_line (self, TAG_STATE, "the natives directory cannot be removed: %s",
              error->message);

  /* The actual state transition happens only after cleanup has stopped touching
   * the instance directory. */
  finish_launch_after_natives (self);
  g_object_unref (self);
}
