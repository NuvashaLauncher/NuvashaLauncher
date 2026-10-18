/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for the launch itself: the states a fake java goes through, the
 * lines it prints, the way it is stopped, the environment it inherits
 * and the log that keeps them.
 *
 * Nothing here touches the network and nothing needs a display. The
 * process is a shell script written into a temporary directory, driven
 * through leppoisa_launcher_spawn(), which is the last step a real launch
 * goes through: the state machine, both output readers and the stop are
 * the ones that run a game. */

#include "leppoisa-launcher.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>

/* ---------------------------------------------------------- *
 * The same async wait the other tests use
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  guint watchdog;
  gboolean timed_out;

  LeppoisaLauncher *launcher;
  LeppoisaLaunchState wanted;
  LeppoisaLaunchState state;
  const char *line; /* what a wait for output looks for */
  char *detail;
  guint changes;
} Watch;

static gboolean
watchdog_fired (gpointer user_data)
{
  Watch *watch = user_data;

  watch->timed_out = TRUE;
  g_main_loop_quit (watch->loop);

  return G_SOURCE_REMOVE;
}

static void
on_state (LeppoisaLauncher *launcher G_GNUC_UNUSED, LeppoisaLaunchState state,
          const char *detail, gpointer user_data)
{
  Watch *watch = user_data;

  watch->state = state;
  watch->changes++;
  g_free (watch->detail);
  watch->detail = g_strdup (detail);

  /* The run ends when the state it waits for arrives, and also when the
     launch is over in any other way: a test that is waiting for
     something else must not hang on a launch that failed. */
  if (state == watch->wanted || !leppoisa_launch_state_is_busy (state))
    g_main_loop_quit (watch->loop);
}

/* Runs the main loop until @wanted comes, or until the launch is over.
   @wanted leppoisa_LAUNCH_STOPPED stands for "the launch is over". */
static void
watch_until (Watch *watch, LeppoisaLauncher *launcher, LeppoisaLaunchState wanted)
{
  watch->launcher = launcher;
  watch->wanted = wanted;
  watch->state = leppoisa_launcher_get_state (launcher);

  /* Everything a launch does before the process is spawned happens
     synchronously, so the state a test waits for is sometimes already
     there before the loop is entered. */
  if (watch->state == wanted || !leppoisa_launch_state_is_busy (watch->state))
    return;

  watch->loop = g_main_loop_new (NULL, FALSE);
  watch->watchdog = g_timeout_add_seconds (60, watchdog_fired, watch);

  g_signal_connect (launcher, "state", G_CALLBACK (on_state), watch);

  g_main_loop_run (watch->loop);

  if (!watch->timed_out)
    g_source_remove (watch->watchdog);

  g_assert_false (watch->timed_out);

  g_signal_handlers_disconnect_by_data (launcher, watch);
  g_main_loop_unref (watch->loop);
  watch->loop = NULL;
}

static void
watch_clear (Watch *watch)
{
  g_clear_pointer (&watch->detail, g_free);
}

static void
on_output (LeppoisaLauncher *launcher G_GNUC_UNUSED, const char *line,
           gboolean stderr G_GNUC_UNUSED, gpointer user_data)
{
  Watch *watch = user_data;

  if (watch->line != NULL && strstr (line, watch->line) != NULL)
    g_main_loop_quit (watch->loop);
}

/* Runs the main loop until a line of the game holds @text. A stop is only
   worth testing once the game has said something: a signal that arrives
   before the script even started would prove nothing about it. */
static void
watch_for_line (Watch *watch, LeppoisaLauncher *launcher, const char *text)
{
  watch->launcher = launcher;
  watch->line = text;
  watch->loop = g_main_loop_new (NULL, FALSE);
  watch->watchdog = g_timeout_add_seconds (60, watchdog_fired, watch);

  g_signal_connect (launcher, "output", G_CALLBACK (on_output), watch);
  g_main_loop_run (watch->loop);

  if (!watch->timed_out)
    g_source_remove (watch->watchdog);

  g_assert_false (watch->timed_out);

  g_signal_handlers_disconnect_by_data (launcher, watch);
  g_main_loop_unref (watch->loop);
  watch->loop = NULL;
}

static void
assert_log_has (LeppoisaLauncher *launcher, const char *text)
{
  g_autofree char *log = leppoisa_launcher_get_log_text (launcher);

  if (g_strstr_len (log, -1, text) == NULL)
    g_error ("the log does not hold \"%s\":\n%s", text, log);
}

/* ---------------------------------------------------------- *
 * A temporary instance and the fake javas
 * ---------------------------------------------------------- */

static char *root;

/* The temporary directory and everything a test wrote in it. */
static void
remove_test_dir (const char *path)
{
  g_autoptr (GPtrArray) children = g_ptr_array_new_with_free_func (g_free);
  GDir *dir = g_dir_open (path, 0, NULL);
  const char *name = NULL;

  if (dir == NULL)
    return;

  while ((name = g_dir_read_name (dir)) != NULL)
    g_ptr_array_add (children, g_build_filename (path, name, NULL));

  g_dir_close (dir);

  for (guint i = 0; i < children->len; i++)
    {
      const char *child = g_ptr_array_index (children, i);

      if (g_file_test (child, G_FILE_TEST_IS_DIR) &&
          !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        remove_test_dir (child);
      else
        g_remove (child);
    }

  g_rmdir (path);
}

static LeppoisaInstance *
new_instance (const char *name)
{
  g_autofree char *directory = g_build_filename (root, name, NULL);
  g_autoptr (GError) error = NULL;
  LeppoisaInstance *instance = NULL;

  if (g_mkdir_with_parents (directory, 0755) != 0)
    g_error ("the instance directory \"%s\" cannot be made", directory);

  instance = leppoisa_instance_new (NULL, name, directory, "1.20.1",
                               leppoisa_LOADER_VANILLA, &error);

  if (instance == NULL)
    g_error ("the test instance cannot be made: %s", error->message);

  return instance;
}

static char *
write_script (const char *name, const char *body)
{
  g_autofree char *path = g_build_filename (root, name, NULL);
  g_autofree char *script = g_strdup_printf ("#!/bin/sh\n%s\n", body);

  if (!g_file_set_contents (path, script, -1, NULL) || g_chmod (path, 0755) != 0)
    g_error ("the fake java at \"%s\" cannot be written", path);

  return g_steal_pointer (&path);
}

/* The command line of a launch that stops at the process: the program
   and nothing else. Takes the path of the program over. */
static GPtrArray *
argv_of (char *program)
{
  GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (argv, program);

  return argv;
}

/* An hour ago, so a crash report of an earlier run looks like one. */
static void
make_an_hour_old (const char *path)
{
  struct utimbuf times;
  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
  time_t when = (time_t) (g_date_time_to_unix (now) - 3600);

  times.actime = when;
  times.modtime = when;

  if (g_utime (path, &times) != 0)
    g_error ("the time of \"%s\" cannot be set", path);
}

/* ---------------------------------------------------------- *
 * A game that prints and ends by itself
 * ---------------------------------------------------------- */

static void
test_output_and_clean_exit (void)
{
  g_autoptr (LeppoisaInstance) instance = new_instance ("clean");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch watch = { 0 };

  argv = argv_of (write_script (
    "clean.sh",
    "echo \"first line\"\n"
    "echo \"second line\" >&2\n"
    "exit 0"));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  g_assert_no_error (error);
  g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_RUNNING);

  watch_until (&watch, launcher, leppoisa_LAUNCH_STOPPED);

  g_assert_cmpint (watch.state, ==, leppoisa_LAUNCH_STOPPED);
  g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_STOPPED);

  /* What the game printed is in the log, tagged and timestamped. */
  assert_log_has (launcher, "[stdout] first line");
  assert_log_has (launcher, "[stderr] second line");
  assert_log_has (launcher, "[state] /");

  /* Every line starts with the time it happened at. */
  {
    g_autofree char *log = leppoisa_launcher_get_log_text (launcher);
    g_autoptr (GRegex) stamp =
      g_regex_new ("^[0-9]{2}:[0-9]{2}:[0-9]{2} \\[", 0, 0, NULL);

    g_assert_nonnull (stamp);
    g_assert_true (g_regex_match (stamp, log, 0, NULL));
  }

  watch_clear (&watch);
}

static void
test_exit_code_makes_a_crash (void)
{
  g_autoptr (LeppoisaInstance) instance = new_instance ("crash");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch watch = { 0 };

  argv = argv_of (write_script ("crash.sh",
                                "echo \"about to die\"\n"
                                "exit 3"));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  watch_until (&watch, launcher, leppoisa_LAUNCH_CRASHED);

  g_assert_cmpint (watch.state, ==, leppoisa_LAUNCH_CRASHED);
  g_assert_cmpstr (watch.detail, ==, "the game exited with code 3");
  assert_log_has (launcher, "the game crashed");

  watch_clear (&watch);
}

static void
test_crash_report_makes_a_crash (void)
{
  g_autofree char *directory = g_build_filename (root, "reported", NULL);
  g_autofree char *reports = g_build_filename (directory, "crash-reports",
                                               NULL);
  g_autofree char *report = g_build_filename (reports,
                                              "crash-2026-10-05_21.00.00-"
                                              "client.txt", NULL);
  g_autoptr (LeppoisaInstance) instance = NULL;
  g_autoptr (LeppoisaLauncher) launcher = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch watch = { 0 };

  g_assert_cmpint (g_mkdir_with_parents (reports, 0755), ==, 0);

  instance = new_instance ("reported");
  launcher = leppoisa_launcher_new (instance, NULL, NULL);

  /* A game that ends with zero still crashed when it wrote a report
     while it was running, which is what the report is for.
     The fake game writes it as it dies, not the test before the launch:
     the launcher compares the modification time against the second the
     process started in, so a report written here would count only when
     this happened to be the same second. */
  {
    g_autofree char *body =
      g_strdup_printf ("printf 'a crash\\n' > '%s'\nexit 0", report);

    argv = argv_of (write_script ("reported.sh", body));
  }

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  watch_until (&watch, launcher, leppoisa_LAUNCH_CRASHED);

  g_assert_cmpint (watch.state, ==, leppoisa_LAUNCH_CRASHED);
  g_assert_cmpstr (watch.detail, ==, "the game wrote a crash report");

  watch_clear (&watch);
}

static void
test_old_crash_report_does_not_count (void)
{
  g_autofree char *directory = g_build_filename (root, "oldreport", NULL);
  g_autofree char *reports = g_build_filename (directory, "crash-reports",
                                               NULL);
  g_autofree char *report = g_build_filename (reports, "crash-old.txt", NULL);
  g_autoptr (LeppoisaInstance) instance = NULL;
  g_autoptr (LeppoisaLauncher) launcher = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch watch = { 0 };

  g_assert_cmpint (g_mkdir_with_parents (reports, 0755), ==, 0);
  g_assert_true (g_file_set_contents (report, "an old crash\n", -1, NULL));

  /* A report of a run before this one says nothing about this one. */
  make_an_hour_old (report);

  instance = new_instance ("oldreport");
  launcher = leppoisa_launcher_new (instance, NULL, NULL);

  argv = argv_of (write_script ("oldreport.sh", "exit 0"));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  watch_until (&watch, launcher, leppoisa_LAUNCH_STOPPED);

  g_assert_cmpint (watch.state, ==, leppoisa_LAUNCH_STOPPED);

  watch_clear (&watch);
}

/* ---------------------------------------------------------- *
 * The environment and the working directory
 * ---------------------------------------------------------- */

static void
test_child_environment (void)
{
  const char *display = g_getenv ("DISPLAY");
  g_autoptr (LeppoisaInstance) instance = new_instance ("environment");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *expected_display =
    g_strdup_printf ("DISPLAY=%s", display != NULL ? display : "unset");
  Watch watch = { 0 };

  /* The launcher process has a backend selection in its environment. The
     game is not leppoisa and must not inherit it. */
  g_setenv ("GDK_BACKEND", "test-backend", TRUE);

  argv = argv_of (write_script (
    "environment.sh",
    "echo \"GDK_BACKEND=${GDK_BACKEND-unset}\"\n"
    "echo \"DISPLAY=${DISPLAY-unset}\"\n"
    "echo \"CWD=$(pwd)\""));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  watch_until (&watch, launcher, leppoisa_LAUNCH_STOPPED);

  assert_log_has (launcher, "[stdout] GDK_BACKEND=unset");

  /* Nothing else about the environment is touched: what the caller
     exported arrives unchanged. */
  assert_log_has (launcher, expected_display);

  /* The game runs in the directory of its instance. */
  {
    g_autofree char *expected =
      g_strdup_printf ("CWD=%s", leppoisa_launcher_get_game_directory (launcher));

    assert_log_has (launcher, expected);
  }

  g_unsetenv ("GDK_BACKEND");

  watch_clear (&watch);
}

static void
test_child_inherits_the_rest_of_the_environment (void)
{
  g_autoptr (LeppoisaInstance) instance = new_instance ("inherited");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch watch = { 0 };

  g_setenv ("leppoisa_TEST_ENVIRONMENT_MARK", "inherited-value", TRUE);

  argv = argv_of (write_script (
    "inherited.sh", "echo \"$leppoisa_TEST_ENVIRONMENT_MARK\""));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  watch_until (&watch, launcher, leppoisa_LAUNCH_STOPPED);

  assert_log_has (launcher, "[stdout] inherited-value");

  g_unsetenv ("leppoisa_TEST_ENVIRONMENT_MARK");

  watch_clear (&watch);
}

/* ---------------------------------------------------------- *
 * One launch per instance
 * ---------------------------------------------------------- */

static void
test_duplicate_launch_prevention (void)
{
  g_autoptr (LeppoisaInstance) instance = new_instance ("single");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch watch = { 0 };

  argv = argv_of (write_script (
    "single.sh",
    "trap 'echo \"stopping\"; exit 0' TERM\n"
    "echo \"running\"\n"
    "while true; do sleep 0.05; done"));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  g_assert_no_error (error);
  g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_RUNNING);

  /* While the process of this instance is up, a second one is refused:
     that is what keeps a game from being started twice. */
  g_assert_false (leppoisa_launcher_spawn (launcher, argv, &error));
  g_assert_error (error, leppoisa_LAUNCH_ERROR, leppoisa_LAUNCH_ERROR_ALREADY_RUNNING);
  g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_RUNNING);

  /* A start of a busy instance does nothing at all: it does not go out
     to the network and it does not change the state. */
  {
    guint before = leppoisa_launcher_get_line_count (launcher);

    leppoisa_launcher_start (launcher);
    g_assert_cmpuint (leppoisa_launcher_get_line_count (launcher), ==, before + 1);
    assert_log_has (launcher, "already Running, nothing was started");
    g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_RUNNING);
  }

  /* The game is stopped so the test leaves nothing behind. */
  leppoisa_launcher_stop (launcher);
  watch_until (&watch, launcher, leppoisa_LAUNCH_STOPPED);

  g_assert_cmpint (watch.state, ==, leppoisa_LAUNCH_STOPPED);

  /* Now that the launch is over the instance is not busy any more, so a
     second launch is allowed and a stop has nothing left to stop. */
  g_assert_false (leppoisa_launch_state_is_busy (leppoisa_launcher_get_state (launcher)));

  {
    guint before = leppoisa_launcher_get_line_count (launcher);

    leppoisa_launcher_stop (launcher);
    g_assert_cmpuint (leppoisa_launcher_get_line_count (launcher), ==, before);
    g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_STOPPED);
  }

  /* Only a running process is refused: a launch that is preparing is
     exactly how the pipeline ends, and a launch that is over is a launch
     that may start again. */
  g_clear_error (&error);
  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  g_assert_no_error (error);
  leppoisa_launcher_stop (launcher);
  watch_until (&watch, launcher, leppoisa_LAUNCH_STOPPED);
  g_assert_cmpint (watch.state, ==, leppoisa_LAUNCH_STOPPED);

  watch_clear (&watch);
}

/* ---------------------------------------------------------- *
 * Stopping
 * ---------------------------------------------------------- */

static void
test_stop_ends_the_game (void)
{
  g_autoptr (LeppoisaInstance) instance = new_instance ("stop");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch running = { 0 };
  Watch stopped = { 0 };

  argv = argv_of (write_script (
    "stop.sh",
    "trap 'echo \"the game was asked to stop\"; exit 0' TERM\n"
    "echo \"the game is running\"\n"
    "while true; do sleep 0.05; done"));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));

  /* Wait for the game to say it is up before asking it to stop. */
  watch_for_line (&running, launcher, "the game is running");
  g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_RUNNING);
  watch_clear (&running);

  leppoisa_launcher_stop (launcher);
  g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_STOPPING);

  watch_until (&stopped, launcher, leppoisa_LAUNCH_STOPPED);

  /* SIGTERM first: the game ends by itself and says so. */
  g_assert_cmpint (stopped.state, ==, leppoisa_LAUNCH_STOPPED);
  assert_log_has (launcher, "asking the game to stop");
  assert_log_has (launcher, "the game was asked to stop");
  assert_log_has (launcher, "the game stopped");

  watch_clear (&stopped);
}

static void
test_stop_forces_a_game_that_ignores_sigterm (void)
{
  g_autoptr (LeppoisaInstance) instance = new_instance ("forced");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch running = { 0 };
  Watch stopped = { 0 };

  /* A game that ignores SIGTERM still has to end: after the timeout it
     is stopped for good, and that is said in the log. */
  argv = argv_of (write_script (
    "forced.sh",
    "trap '' TERM\n"
    "echo \"the game ignores SIGTERM\"\n"
    "while true; do sleep 0.2; done"));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));

  watch_for_line (&running, launcher, "the game ignores SIGTERM");
  watch_clear (&running);

  leppoisa_launcher_stop (launcher);
  watch_until (&stopped, launcher, leppoisa_LAUNCH_STOPPED);

  /* A game that was asked to stop and did not crashed: it stopped. */
  g_assert_cmpint (stopped.state, ==, leppoisa_LAUNCH_STOPPED);
  assert_log_has (launcher, "did not stop within");
  assert_log_has (launcher, "the game stopped");

  watch_clear (&stopped);
}

/* ---------------------------------------------------------- *
 * The log
 * ---------------------------------------------------------- */

static void
test_line_limit_and_clear (void)
{
  g_autoptr (LeppoisaInstance) instance = new_instance ("limit");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GError) error = NULL;
  Watch watch = { 0 };

  /* The default keeps the newest 5000 lines and drops the oldest. */
  g_assert_cmpuint (leppoisa_launcher_get_line_limit (launcher), ==, 5000);

  argv = argv_of (write_script (
    "limit.sh",
    "i=1\n"
    "while [ $i -le 5010 ]; do echo \"line $i\"; i=$((i + 1)); done\n"
    "exit 0"));

  g_assert_true (leppoisa_launcher_spawn (launcher, argv, &error));
  watch_until (&watch, launcher, leppoisa_LAUNCH_STOPPED);

  g_assert_cmpuint (leppoisa_launcher_get_line_count (launcher), ==, 5000);

  /* The oldest lines of the game are gone, the newest are there. */
  assert_log_has (launcher, "[stdout] line 5010");
  assert_log_has (launcher, "[stdout] line 11");

  {
    g_autofree char *log = leppoisa_launcher_get_log_text (launcher);

    g_assert_null (strstr (log, "[stdout] line 10\n"));
  }

  /* Clearing drops everything the launch said so far. */
  leppoisa_launcher_clear_log (launcher);
  g_assert_cmpuint (leppoisa_launcher_get_line_count (launcher), ==, 0);

  {
    g_autofree char *log = leppoisa_launcher_get_log_text (launcher);

    g_assert_cmpstr (log, ==, "");
  }

  watch_clear (&watch);
}

static void
test_spawn_failure_is_a_failure (void)
{
  g_autofree char *missing = g_build_filename (root, "not-a-java", NULL);
  g_autoptr (LeppoisaInstance) instance = new_instance ("missing");
  g_autoptr (LeppoisaLauncher) launcher = leppoisa_launcher_new (instance, NULL, NULL);
  g_autoptr (GPtrArray) argv = argv_of (g_steal_pointer (&missing));
  g_autoptr (GError) error = NULL;

  g_assert_false (leppoisa_launcher_spawn (launcher, argv, &error));
  g_assert_nonnull (error);

  /* Nothing was started: the launch failed and said why. */
  g_assert_cmpint (leppoisa_launcher_get_state (launcher), ==, leppoisa_LAUNCH_FAILED);
  g_assert_false (leppoisa_launch_state_is_busy (leppoisa_launcher_get_state (launcher)));
  assert_log_has (launcher, "not-a-java");
}

int
main (void)
{
  g_print ("# Launch process tests\n");

  root = g_dir_make_tmp ("leppoisa-test-launch-XXXXXX", NULL);

  test_output_and_clean_exit ();
  g_print ("ok - stdout and stderr arrive tagged and timestamped\n");

  test_exit_code_makes_a_crash ();
  g_print ("ok - a non-zero exit is a crash\n");

  test_crash_report_makes_a_crash ();
  g_print ("ok - a crash report of this run is a crash\n");

  test_old_crash_report_does_not_count ();
  g_print ("ok - a crash report of an earlier run is not\n");

  test_child_environment ();
  g_print ("ok - the game does not inherit GDK_BACKEND\n");

  test_child_inherits_the_rest_of_the_environment ();
  g_print ("ok - the game inherits everything else\n");

  test_duplicate_launch_prevention ();
  g_print ("ok - an instance is only ever launched once\n");

  test_stop_ends_the_game ();
  g_print ("ok - a stop ends the game through SIGTERM\n");

  test_stop_forces_a_game_that_ignores_sigterm ();
  g_print ("ok - a game that ignores SIGTERM is stopped for good\n");

  test_line_limit_and_clear ();
  g_print ("ok - the log keeps the newest lines and can be cleared\n");

  test_spawn_failure_is_a_failure ();
  g_print ("ok - a java that cannot be run fails the launch\n");

  remove_test_dir (root);
  g_clear_pointer (&root, g_free);

  return EXIT_SUCCESS;
}
