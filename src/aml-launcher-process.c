/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-launcher-private.h"
#include "aml-i18n.h"
#include "aml-line-reader.h"

#include <signal.h>
#include <string.h>

/* Internal subprocess, output, and crash-report workflow. */

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
open_log_folder (AmlLauncher *self)
{
  g_autofree char *directory = aml_launcher_get_log_directory (self);
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
game_crashed (AmlLauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "the game crashed: %s", reason);
  set_state (self, AML_LAUNCH_CRASHED, reason);

  if (self->settings != NULL &&
      aml_settings_get_open_logs_after_crash (self->settings))
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
  AmlLauncher *self = user_data;
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
    set_state (self, AML_LAUNCH_STOPPED, NULL);

  g_object_unref (self);
}

static void
report_how_it_ended (AmlLauncher *self)
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
static void
finish_launch (AmlLauncher *self)
{
  if (self->process.stop_timeout != 0)
    {
      g_source_remove (self->process.stop_timeout);
      self->process.stop_timeout = 0;
    }

  /* A game that was asked to stop and did, however it did, has not
     crashed: the force stop reports itself in the log instead. */
  if (self->process.stop_requested)
    {
      log_line (self, TAG_STATE, "the game stopped");
      set_state (self, AML_LAUNCH_STOPPED, NULL);
      return;
    }

  if (!self->process.exited_normally || self->process.exit_status != 0)
    {
      report_how_it_ended (self);
      return;
    }

  {
    g_autofree char *directory = aml_launcher_get_crash_report_directory (self);
    g_autoptr (GFile) reports = g_file_new_for_path (directory);

    if (!g_file_query_exists (reports, NULL))
      {
        set_state (self, AML_LAUNCH_STOPPED, NULL);
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

/* ---------------------------------------------------------- *
 * Connect process output to the launcher log
 * ---------------------------------------------------------- */

static void
on_output_line (const char *line, gboolean stderr_line, gpointer user_data)
{
  AmlLauncher *self = user_data;

  log_line (self, stderr_line ? TAG_STDERR : TAG_STDOUT, "%s", line);
}

static void
on_output_done (GError *error, gpointer user_data)
{
  AmlLauncher *self = user_data;

  if (error != NULL)
    log_line (self, TAG_STDOUT, "the output of the game cannot be read: %s",
              error->message);

  if (--self->process.readers == 0 && self->process.exited)
    finish_launch (self);
}

/* Takes the stream it is given over. */
static void
watch_output (AmlLauncher *self, GInputStream *stream, gboolean stderr_line)
{
  AmlLineReader *reader = aml_line_reader_new (
    stream, stderr_line, on_output_line, on_output_done, g_object_ref (self),
    g_object_unref);

  self->process.readers++;
  aml_line_reader_start (reader);
}

/* ---------------------------------------------------------- *
 * The process
 * ---------------------------------------------------------- */

/* The game was asked to stop and did not: it is ended for good. The
 * state still comes out as Stopped, because this stop was asked for and
 * not a crash, but the log says what happened. */
gboolean
on_stop_timeout (gpointer user_data)
{
  AmlLauncher *self = user_data;

  self->process.stop_timeout = 0;

  log_line (self, TAG_STATE,
            "the game did not stop within %d seconds, ending it now",
            AML_LAUNCH_STOP_TIMEOUT);

  if (self->process.process != NULL)
    g_subprocess_force_exit (self->process.process);

  return G_SOURCE_REMOVE;
}

static void
on_process_exited (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlLauncher *self = user_data;

  /* How it ended is read from the process itself: wait_finish() only
     says whether the exit was a clean one. */
  g_subprocess_wait_finish (G_SUBPROCESS (source), result, NULL);

  self->process.exited = TRUE;
  self->process.exited_normally = g_subprocess_get_if_exited (self->process.process);
  self->process.terminated_by_signal = g_subprocess_get_if_signaled (self->process.process);

  if (self->process.exited_normally)
    self->process.exit_status = g_subprocess_get_exit_status (self->process.process);

  if (self->process.terminated_by_signal)
    self->process.term_signal = g_subprocess_get_term_sig (self->process.process);

  g_clear_object (&self->process.process);

  /* The pipes can still hold the last lines of the game, so the launch
     is over once they have been read. */
  if (self->process.readers == 0)
    finish_launch (self);

  g_object_unref (self);
}

gboolean
aml_launcher_spawn (AmlLauncher *self, GPtrArray *argv, GError **error)
{
  g_autoptr (GSubprocessLauncher) launcher = NULL;
  g_autoptr (GError) spawn_error = NULL;
  g_autofree char *shown = NULL;
  g_autoptr (GDateTime) now = NULL;
  char **vector = NULL;
  const char *const *arguments = NULL;

  g_return_val_if_fail (AML_IS_LAUNCHER (self), FALSE);
  g_return_val_if_fail (argv != NULL && argv->len > 0, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* The one rule that keeps a game from being started twice: the pipeline
     holds on to the process from the moment it spawns until the moment
     it is gone, and this is the only place a process is started. A
     launch that is still preparing has nothing to refuse yet, because
     this is how a launch ends. */
  if (self->process.process != NULL)
    {
      g_set_error (error, AML_LAUNCH_ERROR, AML_LAUNCH_ERROR_ALREADY_RUNNING,
                   "this instance is already %s",
                   aml_launch_state_to_string (self->state));
      return FALSE;
    }

  /* GSubprocessLauncher wants a NULL terminated vector, and AML never
     builds a shell line: the arguments go to the process as they are.
     The vector is only the array of pointers: the words themselves
     belong to @argv and are not freed here. */
  vector = g_new0 (char *, argv->len + 1);

  for (guint i = 0; i < argv->len; i++)
    vector[i] = g_ptr_array_index (argv, i);

  arguments = (const char *const *) vector;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_PIPE);

  /* AML runs on Wayland and sets GDK_BACKEND for itself. The game is not
     AML and must not inherit that: it opens its window the way it does
     anywhere else on this desktop, through XWayland. Nothing else about
     the environment is changed, DISPLAY included. */
  g_subprocess_launcher_unsetenv (launcher, "GDK_BACKEND");

  /* The game writes its own files next to the instance: its worlds, its
     logs, its crash reports. */
  g_subprocess_launcher_set_cwd (launcher, self->game_directory);

  set_state (self, AML_LAUNCH_LAUNCHING, "starting the game");

  /* What is about to run, with the token of the session taken out: the
     command line is worth having in the log, the token is not. */
  shown = aml_launch_redact (argv);
  log_line (self, TAG_STATE, "%s", shown);

  self->process.process =
    g_subprocess_launcher_spawnv (launcher, arguments, &spawn_error);

  g_free (vector);

  if (self->process.process == NULL)
    {
      launcher_failed (self, spawn_error->message);
      g_propagate_error (error, g_steal_pointer (&spawn_error));
      return FALSE;
    }

  now = g_date_time_new_now_utc ();
  self->process.started_at = now != NULL ? g_date_time_to_unix (now) : 0;
  self->process.exited = FALSE;
  self->process.exited_normally = FALSE;
  self->process.terminated_by_signal = FALSE;
  self->process.exit_status = 0;
  self->process.term_signal = 0;
  self->process.readers = 0;

  log_line (self, TAG_STATE, "the game runs as process %s",
            g_subprocess_get_identifier (self->process.process));

  watch_output (self, g_subprocess_get_stdout_pipe (self->process.process), FALSE);
  watch_output (self, g_subprocess_get_stderr_pipe (self->process.process), TRUE);

  g_subprocess_wait_async (self->process.process, NULL, on_process_exited,
                           g_object_ref (self));

  /* The process is up: that is the event the Running state comes from. */
  set_state (self, AML_LAUNCH_RUNNING, NULL);

  return TRUE;
}
