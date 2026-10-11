/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <gio/gio.h>
#include <glib.h>

#include "aml-task.h"

typedef struct {
  AmlTaskSequence *sequence;
  guint calls;
  gboolean data_freed;
} SequenceData;

typedef struct {
  GMainLoop *loop;
  char *answer;
} SequenceResult;

static void
sequence_data_free (gpointer user_data)
{
  SequenceData *data = user_data;

  data->data_freed = TRUE;
}

static void
sequence_step (GTask *task G_GNUC_UNUSED, gpointer user_data)
{
  SequenceData *data = user_data;

  data->calls++;

  if (data->calls == 1)
    {
      aml_task_sequence_next (data->sequence);
      return;
    }

  aml_task_sequence_return_pointer (data->sequence, g_strdup ("complete"),
                                    g_free);
}

static void
sequence_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  SequenceResult *result_data = user_data;
  g_autoptr (GError) error = NULL;

  result_data->answer =
    g_task_propagate_pointer (G_TASK (result), &error);
  g_assert_no_error (error);
  g_main_loop_quit (result_data->loop);
}

static void
test_sequence_progresses_and_cleans_up (void)
{
  SequenceData data = { 0 };
  SequenceResult result = { 0 };

  result.loop = g_main_loop_new (NULL, FALSE);

  data.sequence = aml_task_sequence_new (NULL, sequence_done, &result,
                                          sequence_step, &data,
                                          sequence_data_free);
  aml_task_sequence_start (data.sequence);
  g_main_loop_run (result.loop);

  g_assert_cmpuint (data.calls, ==, 2);
  g_assert_true (data.data_freed);
  g_assert_cmpstr (result.answer, ==, "complete");
  g_free (result.answer);
  g_main_loop_unref (result.loop);
  g_print ("ok - task sequence advances, returns results and frees state\n");
}

static void
error_step (GTask *task G_GNUC_UNUSED, gpointer user_data)
{
  SequenceData *data = user_data;

  data->calls++;
  aml_task_sequence_return_error (
    data->sequence,
    g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "step failed"));
}

static void
error_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  SequenceResult *result_data = user_data;
  g_autoptr (GError) error = NULL;
  gpointer value = g_task_propagate_pointer (G_TASK (result), &error);

  g_assert_null (value);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_main_loop_quit (result_data->loop);
}

static void
test_sequence_returns_errors (void)
{
  SequenceData data = { 0 };
  SequenceResult result = { 0 };

  result.loop = g_main_loop_new (NULL, FALSE);
  data.sequence = aml_task_sequence_new (NULL, error_done, &result,
                                          error_step, &data,
                                          sequence_data_free);
  aml_task_sequence_start (data.sequence);
  g_main_loop_run (result.loop);

  g_assert_cmpuint (data.calls, ==, 1);
  g_assert_true (data.data_freed);
  g_main_loop_unref (result.loop);
  g_print ("ok - task sequence returns errors and frees state\n");
}

static void
cancel_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  SequenceResult *result_data = user_data;
  g_autoptr (GError) error = NULL;
  gpointer value = g_task_propagate_pointer (G_TASK (result), &error);

  g_assert_null (value);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_main_loop_quit (result_data->loop);
}

static void
test_sequence_honors_cancellation (void)
{
  SequenceData data = { 0 };
  SequenceResult result = { 0 };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();

  result.loop = g_main_loop_new (NULL, FALSE);
  g_cancellable_cancel (cancellable);
  data.sequence = aml_task_sequence_new (cancellable, cancel_done, &result,
                                          sequence_step, &data,
                                          sequence_data_free);
  aml_task_sequence_start (data.sequence);
  g_main_loop_run (result.loop);

  g_assert_cmpuint (data.calls, ==, 0);
  g_assert_true (data.data_freed);
  g_main_loop_unref (result.loop);
  g_print ("ok - task sequence stops before a cancelled step\n");
}

static gboolean
scope_probe (gpointer user_data)
{
  gboolean *ran = user_data;
  *ran = TRUE;
  return G_SOURCE_REMOVE;
}

static gboolean
scope_stress_probe (gpointer user_data)
{
  guint *callbacks = user_data;
  (*callbacks)++;
  return G_SOURCE_REMOVE;
}

/* Everything below is about a source that has already fired.
 * ---------------------------------------------------------- */

/* GLib does not complain when a live source is removed: that is what
 * removal is for. It complains when the id no longer names a source,
 * because then the main loop's bookkeeping is being told to undo
 * something that was never recorded. A tracked id outlives its source,
 * so a scope that removes sources on the way out has to check that the
 * source is still there first, and these tests are what proves it does.
 *
 * The counter catches the complaint the same way test-window does. */
static guint criticals;

static GLogWriterOutput
count_criticals (GLogLevelFlags   log_level,
                 const GLogField *fields,
                 gsize            n_fields,
                 gpointer         user_data)
{
  (void) user_data;

  if ((log_level & G_LOG_LEVEL_CRITICAL) != 0)
    criticals++;

  return g_log_writer_default (log_level, fields, n_fields, NULL);
}

static gboolean
already_fired_probe (gpointer user_data)
{
  guint *fired = user_data;
  (*fired)++;
  return G_SOURCE_REMOVE;
}

/* Hands back the id of a source that has run and been destroyed, so the
 * id names nothing by the time it is returned. */
static guint
fire_and_dissolve (guint *fired)
{
  guint source = g_timeout_add (1, already_fired_probe, fired);

  while (*fired == 0)
    g_main_context_iteration (NULL, FALSE);

  g_assert_cmpuint (*fired, ==, 1);
  g_assert_null (g_main_context_find_source_by_id (NULL, source));

  return source;
}

static void
test_scope_free_releases_a_source_that_fired (void)
{
  guint fired = 0;
  guint before = criticals;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  AmlTaskScope *scope = aml_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = fire_and_dissolve (&fired);

  aml_task_scope_track_source (scope, source);
  aml_task_scope_free (scope);

  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - freeing a scope releases a source that already fired\n");
}

static void
test_scope_cancel_releases_a_source_that_fired (void)
{
  guint fired = 0;
  guint before = criticals;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  AmlTaskScope *scope = aml_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = fire_and_dissolve (&fired);

  aml_task_scope_track_source (scope, source);
  aml_task_scope_cancel (scope);
  aml_task_scope_free (scope);

  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - cancelling a scope releases a source that already fired\n");
}

static void
test_scope_removes_a_source_that_fired (void)
{
  guint fired = 0;
  guint before = criticals;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  AmlTaskScope *scope = aml_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = fire_and_dissolve (&fired);

  aml_task_scope_track_source (scope, source);
  aml_task_scope_remove_source (scope, source);
  aml_task_scope_free (scope);

  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - removing one spent source leaves the rest of the scope\n");
}

/* Both shapes at once: some sources have fired and some have not, which
   is what a scope really looks like when it is torn down part way
   through. Removing the live ones must not disturb the spent ones. */
static void
test_scope_releases_mixed_sources (void)
{
  guint fired = 0;
  gboolean live_ran = FALSE;
  guint before = criticals;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  AmlTaskScope *scope = aml_task_scope_new (NULL, cancellable, NULL, NULL);
  guint spent = fire_and_dissolve (&fired);
  guint live = g_idle_add (scope_probe, &live_ran);

  aml_task_scope_track_source (scope, spent);
  aml_task_scope_track_source (scope, live);
  aml_task_scope_free (scope);

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_false (live_ran);
  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - a scope with both spent and live sources cleans up once\n");
}

static void
test_scope_cancels_owned_sources (void)
{
  gboolean ran = FALSE;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  AmlTaskScope *scope = aml_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = g_idle_add (scope_probe, &ran);

  aml_task_scope_track_source (scope, source);
  aml_task_scope_cancel (scope);
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_false (ran);
  aml_task_scope_free (scope);
  g_print ("ok - task scopes cancel and release owned sources\n");
}

static void
test_scope_cancellation_stress (void)
{
  guint callbacks = 0;

  for (guint i = 0; i < 1000; i++)
    {
      g_autoptr (GCancellable) cancellable = g_cancellable_new ();
      AmlTaskScope *scope = aml_task_scope_new (NULL, cancellable, NULL,
                                                NULL);
      guint source = g_idle_add_full (
        G_PRIORITY_DEFAULT, scope_stress_probe, &callbacks, NULL);

      aml_task_scope_track_source (scope, source);
      aml_task_scope_cancel (scope);
      aml_task_scope_free (scope);
    }

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_cmpuint (callbacks, ==, 0);
  g_print ("ok - task scope cancellation remains leak-free under stress\n");
}

/* A step that starts nothing and finishes nothing, so the test decides
 * when and how the sequence completes. */
static void
manual_step (GTask *task G_GNUC_UNUSED, gpointer user_data)
{
  guint *steps = user_data;
  (*steps)++;
}

/* Completing a sequence twice used to read the finished flag out of freed
 * memory: the flag was written and the struct freed together, so it could
 * never be seen as set. The sequence now lives as long as the task, which
 * is what makes the flag mean anything at all. */
static void
test_sequence_survives_completing_twice (void)
{
  guint steps = 0;
  SequenceResult result = { 0 };
  AmlTaskSequence *sequence;
  g_autofree char *first = g_strdup ("the one that counts");

  result.loop = g_main_loop_new (NULL, FALSE);

  sequence = aml_task_sequence_new (NULL, sequence_done, &result, manual_step,
                                    &steps, NULL);
  aml_task_sequence_start (sequence);
  g_assert_cmpuint (steps, ==, 1);

  aml_task_sequence_return_pointer (sequence, g_steal_pointer (&first),
                                    g_free);

  /* Every way a late completion can arrive, each of which has to stop
     rather than act on state that is gone. The results handed to the
     ones that stop have to be released, which is what the result_free
     and the error are for. */
  aml_task_sequence_return_pointer (sequence, g_strdup ("again"), g_free);
  aml_task_sequence_return_error (sequence,
                                  g_error_new_literal (G_IO_ERROR,
                                                       G_IO_ERROR_FAILED,
                                                       "again"));
  aml_task_sequence_next (sequence);

  g_main_loop_run (result.loop);

  /* One step, one completion, and the value the first one carried. */
  g_assert_cmpuint (steps, ==, 1);
  g_assert_cmpstr (result.answer, ==, "the one that counts");
  g_free (result.answer);

  g_main_loop_unref (result.loop);
  g_print ("ok - a sequence completed twice stays finished\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_log_set_writer_func (count_criticals, NULL, NULL);

  g_print ("# Task sequence\n");
  test_sequence_progresses_and_cleans_up ();
  test_sequence_returns_errors ();
  test_sequence_honors_cancellation ();
  test_sequence_survives_completing_twice ();
  test_scope_cancels_owned_sources ();
  test_scope_cancellation_stress ();

  g_print ("# Task scope and sources it has already lost\n");
  test_scope_free_releases_a_source_that_fired ();
  test_scope_cancel_releases_a_source_that_fired ();
  test_scope_removes_a_source_that_fired ();
  test_scope_releases_mixed_sources ();
  return 0;
}
