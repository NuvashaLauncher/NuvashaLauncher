/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-launcher-private.h"
#include "nuvasha-i18n.h"
#include "nuvasha-line-reader.h"

#include <signal.h>
#include <string.h>

/* Internal subprocess, output, and crash-report workflow. */

/* ---------------------------------------------------------- *
 * Connect process output to the launcher log
 * ---------------------------------------------------------- */

static void
on_output_line (const char *line, gboolean stderr_line, gpointer user_data)
{
  NuvashaLauncher *self = nuvasha_LAUNCHER (user_data);

  log_line (self, stderr_line ? TAG_STDERR : TAG_STDOUT, "%s", line);
}

static void
on_output_done (GError *error, gpointer user_data)
{
  NuvashaLauncher *self = nuvasha_LAUNCHER (user_data);

  if (error != NULL)
    log_line (self, TAG_STDOUT, "the output of the game cannot be read: %s",
              error->message);

  if (--self->process.readers == 0 && self->process.exited)
    finish_launch (self);
}

/* Takes the stream it is given over. */
static void
watch_output (NuvashaLauncher *self, GInputStream *stream, gboolean stderr_line)
{
  NuvashaLineReader *reader = nuvasha_line_reader_new (
    stream, stderr_line, on_output_line, on_output_done, g_object_ref (self),
    g_object_unref);

  self->process.readers++;
  nuvasha_line_reader_start (reader);
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
  NuvashaLauncher *self = nuvasha_LAUNCHER (user_data);
  guint source_id = self->process.stop_timeout;

  self->process.stop_timeout = 0;
  nuvasha_task_scope_forget_source (self->scope, source_id);

  log_line (self, TAG_STATE,
            "the game did not stop within %d seconds, ending it now",
            nuvasha_LAUNCH_STOP_TIMEOUT);

  if (self->process.process != NULL)
    g_subprocess_force_exit (self->process.process);

  return G_SOURCE_REMOVE;
}

static void
on_process_exited (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncher *self = nuvasha_LAUNCHER (user_data);

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
nuvasha_launcher_spawn (NuvashaLauncher *self, GPtrArray *argv, GError **error)
{
  g_autoptr (GSubprocessLauncher) launcher = NULL;
  g_autoptr (GError) spawn_error = NULL;
  g_autofree char *shown = NULL;
  g_autoptr (GDateTime) now = NULL;
  char **vector = NULL;
  const char *const *arguments = NULL;

  g_return_val_if_fail (nuvasha_IS_LAUNCHER (self), FALSE);
  g_return_val_if_fail (argv != NULL && argv->len > 0, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* The one rule that keeps a game from being started twice: the pipeline
     holds on to the process from the moment it spawns until the moment
     it is gone, and this is the only place a process is started. A
     launch that is still preparing has nothing to refuse yet, because
     this is how a launch ends. */
  if (self->process.process != NULL)
    {
      g_set_error (error, nuvasha_LAUNCH_ERROR, nuvasha_LAUNCH_ERROR_ALREADY_RUNNING,
                   "this instance is already %s",
                   nuvasha_launch_state_to_string (self->state));
      return FALSE;
    }

  /* GSubprocessLauncher wants a NULL terminated vector, and nuvasha never
     builds a shell line: the arguments go to the process as they are.
     The vector is only the array of pointers: the words themselves
     belong to @argv and are not freed here. */
  vector = g_new0 (char *, argv->len + 1);

  for (guint i = 0; i < argv->len; i++)
    vector[i] = g_ptr_array_index (argv, i);

  arguments = (const char *const *) vector;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_PIPE);

  /* The game is not nuvasha and must not inherit a GDK backend selection. Nothing
     else about the environment is changed, DISPLAY included. */
  g_subprocess_launcher_unsetenv (launcher, "GDK_BACKEND");

  /* The game writes its own files next to the instance: its worlds, its
     logs, its crash reports. */
  g_subprocess_launcher_set_cwd (launcher, self->game_directory);

  set_state (self, nuvasha_LAUNCH_LAUNCHING, "starting the game");

  /* What is about to run, with the token of the session taken out: the
     command line is worth having in the log, the token is not. */
  shown = nuvasha_launch_redact (argv);
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
  set_state (self, nuvasha_LAUNCH_RUNNING, NULL);

  return TRUE;
}
