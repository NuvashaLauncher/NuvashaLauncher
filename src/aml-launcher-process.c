/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "aml-launcher-private.h"
#include "aml-i18n.h"
#include "aml-line-reader.h"
#include "aml-sandbox.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <gio/gunixinputstream.h>

/* Internal subprocess, output, and crash-report workflow. */

/* ---------------------------------------------------------- *
 * Connect process output to the launcher log
 * ---------------------------------------------------------- */

static void
on_output_line (const char *line, gboolean stderr_line, gpointer user_data)
{
  AmlLauncher *self = AML_LAUNCHER (user_data);

  log_line (self, stderr_line ? TAG_STDERR : TAG_STDOUT, "%s", line);
}

static void
on_output_done (GError *error, gpointer user_data)
{
  AmlLauncher *self = AML_LAUNCHER (user_data);

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
  AmlLauncher *self = AML_LAUNCHER (user_data);
  guint source_id = self->process.stop_timeout;

  self->process.stop_timeout = 0;
  aml_task_scope_forget_source (self->scope, source_id);

  log_line (self, TAG_STATE,
            "the game did not stop within %d seconds, ending it now",
            AML_LAUNCH_STOP_TIMEOUT);

  if (self->process.process != NULL)
    g_subprocess_force_exit (self->process.process);

  return G_SOURCE_REMOVE;
}

static void
open_request_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlLauncher *self = AML_LAUNCHER (user_data);
  g_autoptr (GError) error = NULL;
  g_autofree char *request =
    g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (source),
                                         result, NULL, &error);

  if (request != NULL)
    {
      if (g_str_has_prefix (request, "/instance/") &&
          strstr (request, "..") == NULL)
        {
          g_autofree char *relative = g_strdup (request + strlen ("/instance/"));
          g_autofree char *path = g_build_filename (self->game_directory,
                                                    relative, NULL);
          const char *gio_argv[] = { "/usr/bin/gio", "open", path, NULL };

          log_line (self, TAG_STATE, "opening sandbox request: %s", relative);

          g_autoptr (GSubprocess) opener =
            g_subprocess_newv (gio_argv, G_SUBPROCESS_FLAGS_NONE, &error);
          if (opener == NULL)
            log_line (self, TAG_STATE, "cannot open %s: %s", relative,
                      error->message);
          else
            log_line (self, TAG_STATE, "host opener started for %s", relative);
        }
      else
        log_line (self, TAG_STATE, "rejected sandbox open request: %s", request);
    }

  if (error == NULL && request != NULL)
    g_data_input_stream_read_line_async (G_DATA_INPUT_STREAM (source),
                                         G_PRIORITY_DEFAULT, NULL,
                                         open_request_done, self);
  else
    {
      if (G_DATA_INPUT_STREAM (source) == self->process.open_stream)
        g_clear_object (&self->process.open_stream);
      else if (G_DATA_INPUT_STREAM (source) == self->process.open_fifo_stream)
        g_clear_object (&self->process.open_fifo_stream);
    }
}

static void
watch_open_requests (AmlLauncher *self, int fd, gboolean fifo)
{
  GDataInputStream *stream = g_data_input_stream_new (
    g_unix_input_stream_new (fd, TRUE));

  if (fifo)
    self->process.open_fifo_stream = stream;
  else
    self->process.open_stream = stream;

  g_data_input_stream_read_line_async (stream,
                                       G_PRIORITY_DEFAULT, NULL,
                                       open_request_done, self);
}

static void
on_process_exited (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlLauncher *self = AML_LAUNCHER (user_data);

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
  g_autoptr (GPtrArray) launch_argv = NULL;
  char **vector = NULL;
  const char *const *arguments = NULL;
  gboolean sandboxed = FALSE;
  int seccomp_fd = -1;
  int open_pipe[2] = { -1, -1 };
  int open_fifo_fd = -1;
  int broker_fd = -1;
  int broker_gio_fd = -1;

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

  if (self->settings != NULL &&
      aml_settings_get_sandbox_enabled (self->settings))
    {
      g_autofree char *runtime = g_build_filename (self->game_directory,
                                                   ".aml-runtime", NULL);
      g_autofree char *fifo = g_build_filename (runtime, "open-request", NULL);
      struct stat info;

      if (g_mkdir_with_parents (runtime, 0700) != 0 ||
          (lstat (fifo, &info) == 0 && !S_ISFIFO (info.st_mode)) ||
          (lstat (fifo, &info) != 0 && mkfifo (fifo, 0600) != 0))
        {
          launcher_failed (self, "the sandbox open request channel cannot be created");
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                       "cannot create the sandbox open request channel");
          return FALSE;
        }
      open_fifo_fd = open (fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
      if (open_fifo_fd < 0)
        {
          launcher_failed (self, "the sandbox open request channel cannot be opened");
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "cannot open the sandbox open request channel: %s",
                       g_strerror (errno));
          return FALSE;
        }
    }

  if (self->settings != NULL &&
      aml_settings_get_sandbox_enabled (self->settings))
    {
      g_autoptr (GError) sandbox_error = NULL;

      launch_argv = aml_sandbox_build_arguments (
        argv, self->game_directory, self->preparation.resolved,
        self->preparation.java_executable,
        aml_settings_get_sandbox_network (self->settings), &seccomp_fd,
         pipe (open_pipe) == 0 ? 4 : -1,
         &broker_fd,
         &broker_gio_fd,
         &sandbox_error);

      if (launch_argv == NULL && sandbox_error != NULL &&
          g_error_matches (sandbox_error, AML_SANDBOX_ERROR,
                           AML_SANDBOX_ERROR_UNAVAILABLE))
        {
          log_line (self, TAG_STATE,
                    "Bubblewrap is unavailable; launching without sandboxing: %s",
                    sandbox_error->message);
        }
      else if (launch_argv == NULL)
        {
          if (open_pipe[0] >= 0)
            close (open_pipe[0]);
          if (open_pipe[1] >= 0)
            close (open_pipe[1]);
          if (broker_fd >= 0)
            close (broker_fd);
          if (broker_gio_fd >= 0)
            close (broker_gio_fd);
          launcher_failed (self, sandbox_error != NULL
                                   ? sandbox_error->message
                                   : "the sandbox could not be created");
          g_propagate_error (error, g_steal_pointer (&sandbox_error));
          return FALSE;
        }
      else
        sandboxed = TRUE;
    }

  if (!sandboxed)
    {
      if (open_pipe[0] >= 0)
        close (open_pipe[0]);
      if (open_pipe[1] >= 0)
        close (open_pipe[1]);
      if (broker_fd >= 0)
        close (broker_fd);
      if (open_fifo_fd >= 0)
        close (open_fifo_fd);
      if (broker_gio_fd >= 0)
        close (broker_gio_fd);
      open_pipe[0] = open_pipe[1] = -1;
      launch_argv = g_ptr_array_ref (argv);
    }

  /* GSubprocessLauncher wants a NULL terminated vector, and AML never
      builds a shell line: the arguments go to the process as they are.
      The vector is only the array of pointers: the words themselves
      belong to @launch_argv and are not freed here. */
  vector = g_new0 (char *, launch_argv->len + 1);

  for (guint i = 0; i < launch_argv->len; i++)
    vector[i] = g_ptr_array_index (launch_argv, i);

  arguments = (const char *const *) vector;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_PIPE);

  if (seccomp_fd >= 0)
    g_subprocess_launcher_take_fd (launcher, seccomp_fd, 3);
  if (sandboxed && open_pipe[0] >= 0)
    {
      g_subprocess_launcher_take_fd (launcher, open_pipe[1], 4);
      open_pipe[1] = -1;
    }
  if (sandboxed && broker_fd >= 0)
    {
      g_subprocess_launcher_take_fd (launcher, broker_fd, 5);
      broker_fd = -1;
    }
  if (sandboxed && broker_gio_fd >= 0)
    {
      g_subprocess_launcher_take_fd (launcher, broker_gio_fd, 6);
      broker_gio_fd = -1;
    }

  /* The game is not AML and must not inherit a GDK backend selection. Nothing
     else about the environment is changed, DISPLAY included. */
  g_subprocess_launcher_unsetenv (launcher, "GDK_BACKEND");

  /* The game writes its own files next to the instance: its worlds, its
     logs, its crash reports. */
  g_subprocess_launcher_set_cwd (launcher, self->game_directory);

  set_state (self, AML_LAUNCH_LAUNCHING, "starting the game");

  if (sandboxed)
    log_line (self, TAG_STATE,
              "the game is starting inside a Bubblewrap sandbox");

  /* What is about to run, with the token of the session taken out: the
     command line is worth having in the log, the token is not. */
  shown = aml_launch_redact (argv);
  log_line (self, TAG_STATE, "%s", shown);

  self->process.process =
    g_subprocess_launcher_spawnv (launcher, arguments, &spawn_error);

  g_free (vector);

  if (self->process.process == NULL)
    {
      if (open_pipe[0] >= 0)
        close (open_pipe[0]);
      if (open_pipe[1] >= 0)
        close (open_pipe[1]);
      if (broker_fd >= 0)
        close (broker_fd);
      if (open_fifo_fd >= 0)
        close (open_fifo_fd);
      if (broker_gio_fd >= 0)
        close (broker_gio_fd);
      launcher_failed (self, spawn_error->message);
      g_propagate_error (error, g_steal_pointer (&spawn_error));
      return FALSE;
    }

  if (sandboxed && open_pipe[0] >= 0)
    {
      watch_open_requests (self, open_pipe[0], FALSE);
      open_pipe[0] = -1;
    }
  if (sandboxed && open_fifo_fd >= 0)
    {
      watch_open_requests (self, open_fifo_fd, TRUE);
      open_fifo_fd = -1;
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
