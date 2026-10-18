/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-launcher-private.h"
#include "leppoisa-fs.h"
#include "leppoisa-i18n.h"

/* ---------------------------------------------------------- *
 * Where the game ends
 * ---------------------------------------------------------- */

/* The desktop says no when it wants to. Nothing about that is worth a
   word in the log of a game: the folder is a convenience, and the button
   that asks for it is there for the next try. */
static void
log_folder_opened (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data G_GNUC_UNUSED)
{
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_default_for_uri_finish (result, &error))
    g_debug ("the log folder does not open: %s", error->message);
}

static void
open_log_folder (LeppoisaLauncher *self)
{
  g_autofree char *directory = leppoisa_launcher_get_log_directory (self);
  g_autofree char *uri = NULL;

  /* A game that never got far enough to write a log has no folder to
     open, which is not worth a word in the log. */
  if (!g_file_test (directory, G_FILE_TEST_IS_DIR))
    return;

  uri = g_filename_to_uri (directory, NULL, NULL);

  if (uri == NULL)
    return;

  /* GIO hands the folder to the desktop the same way a double click in
     a file manager does; nothing is run through a shell. */
  g_app_info_launch_default_for_uri_async (uri, NULL, NULL,
                                           log_folder_opened, NULL);
}

static void
game_crashed (LeppoisaLauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "the game crashed: %s", reason);
  set_state (self, leppoisa_LAUNCH_CRASHED, reason);

  if (self->settings != NULL &&
      leppoisa_settings_get_open_logs_after_crash (self->settings))
    open_log_folder (self);
}

/* Nothing else can make a game that ended with zero a crash except a
 * report it wrote while it was dying, so that is what is asked for. A
 * report of an earlier run does not count: the launch time is what the
 * modification time is compared against. */
static void
crash_reports_listed (GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
  LeppoisaLauncher *self = leppoisa_LAUNCHER (user_data);
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
            written_now =
              g_file_info_get_attribute_uint64 (
                info, G_FILE_ATTRIBUTE_TIME_MODIFIED) >= (guint64) self->process.started_at;

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
    game_crashed (self, "the game wrote a crash report");
  else
    set_state (self, leppoisa_LAUNCH_STOPPED, NULL);

  g_object_unref (self);
}

static void
report_how_it_ended (LeppoisaLauncher *self)
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
finish_launch (LeppoisaLauncher *self)
{
  /* Natives are launch artifacts, not instance data. They are extracted from
   * the cached jars again on the next launch, so removing this per-instance
   * directory prevents a game or mod from leaving native code behind. */
  {
    g_autofree char *natives =
      g_build_filename (self->game_directory, "natives", NULL);
    leppoisa_fs_remove_tree (natives);
  }

  if (self->process.stop_timeout != 0)
    {
      /* Through the scope, which is where this source is registered. */
      leppoisa_task_scope_remove_source (self->scope, self->process.stop_timeout);
      self->process.stop_timeout = 0;
    }

  /* A game that was asked to stop and did, however it did, has not
     crashed: the force stop reports itself in the log instead. */
  if (self->process.stop_requested)
    {
      log_line (self, TAG_STATE, "the game stopped");
      set_state (self, leppoisa_LAUNCH_STOPPED, NULL);
      return;
    }

  if (!self->process.exited_normally || self->process.exit_status != 0)
    {
      report_how_it_ended (self);
      return;
    }

  {
    g_autofree char *directory = leppoisa_launcher_get_crash_report_directory (self);
    g_autoptr (GFile) reports = g_file_new_for_path (directory);

    if (!g_file_query_exists (reports, NULL))
      {
        set_state (self, leppoisa_LAUNCH_STOPPED, NULL);
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
