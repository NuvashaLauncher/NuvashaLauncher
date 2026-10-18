/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <gio/gio.h>
#include <glib.h>

#include "comfy-task.h"

typedef struct {
  ComfyTaskSequence *sequence;
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
sequence_step (ComfyTaskSequence *sequence G_GNUC_UNUSED, gpointer user_data)
{
  SequenceData *data = user_data;

  data->calls++;

  if (data->calls == 1)
    {
      comfy_task_sequence_next (data->sequence);
      return;
    }

  comfy_task_sequence_return_pointer (data->sequence, g_strdup ("complete"),
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

  data.sequence = comfy_task_sequence_new (NULL, sequence_done, &result,
                                          sequence_step, &data,
                                          sequence_data_free);
  comfy_task_sequence_start (data.sequence);
  comfy_task_sequence_unref (data.sequence);
  g_main_loop_run (result.loop);

  g_assert_cmpuint (data.calls, ==, 2);
  g_assert_true (data.data_freed);
  g_assert_cmpstr (result.answer, ==, "complete");
  g_free (result.answer);
  g_main_loop_unref (result.loop);
  g_print ("ok - task sequence advances, returns results and frees state\n");
}

static void
test_scope_ids_and_states (void)
{
  g_autoptr (ComfyTaskScope) first = comfy_task_scope_new (NULL, NULL, NULL, NULL);
  g_autoptr (ComfyTaskScope) second = comfy_task_scope_new (NULL, NULL, NULL, NULL);

  g_assert_cmpuint (comfy_task_scope_get_id (first), >, 0);
  g_assert_cmpuint (comfy_task_scope_get_id (first), !=,
                    comfy_task_scope_get_id (second));
  g_assert_cmpint (comfy_task_scope_get_state (first), ==,
                   comfy_TASK_STATE_QUEUED);

  comfy_task_scope_set_state (first, comfy_TASK_STATE_RUNNING);
  g_assert_cmpint (comfy_task_scope_get_state (first), ==,
                   comfy_TASK_STATE_RUNNING);
  comfy_task_scope_cancel (first);
  g_assert_cmpint (comfy_task_scope_get_state (first), ==,
                   comfy_TASK_STATE_CANCELLED);

  comfy_task_scope_return_boolean (second, TRUE);
  g_assert_cmpint (comfy_task_scope_get_state (second), ==,
                   comfy_TASK_STATE_COMPLETED);
  g_print ("ok - operation ids and lifecycle states are observable\n");
}

static void
error_step (ComfyTaskSequence *sequence G_GNUC_UNUSED, gpointer user_data)
{
  SequenceData *data = user_data;

  data->calls++;
  comfy_task_sequence_return_error (
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
  data.sequence = comfy_task_sequence_new (NULL, error_done, &result,
                                          error_step, &data,
                                          sequence_data_free);
  comfy_task_sequence_start (data.sequence);
  comfy_task_sequence_unref (data.sequence);
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
  data.sequence = comfy_task_sequence_new (cancellable, cancel_done, &result,
                                          sequence_step, &data,
                                          sequence_data_free);
  comfy_task_sequence_start (data.sequence);
  comfy_task_sequence_unref (data.sequence);
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
  ComfyTaskScope *scope = comfy_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = fire_and_dissolve (&fired);

  comfy_task_scope_track_source (scope, source);
  comfy_task_scope_unref (scope);

  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - freeing a scope releases a source that already fired\n");
}

static void
test_scope_cancel_releases_a_source_that_fired (void)
{
  guint fired = 0;
  guint before = criticals;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  ComfyTaskScope *scope = comfy_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = fire_and_dissolve (&fired);

  comfy_task_scope_track_source (scope, source);
  comfy_task_scope_cancel (scope);
  comfy_task_scope_unref (scope);

  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - cancelling a scope releases a source that already fired\n");
}

static void
test_scope_removes_a_source_that_fired (void)
{
  guint fired = 0;
  guint before = criticals;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  ComfyTaskScope *scope = comfy_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = fire_and_dissolve (&fired);

  comfy_task_scope_track_source (scope, source);
  comfy_task_scope_remove_source (scope, source);
  comfy_task_scope_unref (scope);

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
  ComfyTaskScope *scope = comfy_task_scope_new (NULL, cancellable, NULL, NULL);
  guint spent = fire_and_dissolve (&fired);
  guint live = g_idle_add (scope_probe, &live_ran);

  comfy_task_scope_track_source (scope, spent);
  comfy_task_scope_track_source (scope, live);
  comfy_task_scope_unref (scope);

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
  ComfyTaskScope *scope = comfy_task_scope_new (NULL, cancellable, NULL, NULL);
  guint source = g_idle_add (scope_probe, &ran);

  comfy_task_scope_track_source (scope, source);
  comfy_task_scope_cancel (scope);
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_false (ran);
  comfy_task_scope_unref (scope);
  g_print ("ok - task scopes cancel and release owned sources\n");
}

static void
test_scope_cancellation_stress (void)
{
  guint callbacks = 0;

  for (guint i = 0; i < 1000; i++)
    {
      g_autoptr (GCancellable) cancellable = g_cancellable_new ();
      ComfyTaskScope *scope = comfy_task_scope_new (NULL, cancellable, NULL,
                                                NULL);
      guint source = g_idle_add_full (
        G_PRIORITY_DEFAULT, scope_stress_probe, &callbacks, NULL);

      comfy_task_scope_track_source (scope, source);
      comfy_task_scope_cancel (scope);
      comfy_task_scope_unref (scope);
    }

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_cmpuint (callbacks, ==, 0);
  g_print ("ok - task scope cancellation remains leak-free under stress\n");
}

static void
test_scope_reference_keeps_operation_alive (void)
{
  ComfyTaskScope *scope = comfy_task_scope_new (NULL, NULL, NULL, NULL);
  ComfyTaskScope *held = comfy_task_scope_ref (scope);
  g_autoptr (GCancellable) cancellable =
    g_object_ref (comfy_task_scope_get_cancellable (held));

  comfy_task_scope_unref (scope);
  g_assert_true (g_cancellable_is_cancelled (cancellable) == FALSE);
  comfy_task_scope_cancel (held);
  g_assert_true (g_cancellable_is_cancelled (cancellable));
  comfy_task_scope_unref (held);

  g_print ("ok - task scope references keep operation state alive\n");
}

/* The handle an operation hands back has to be releasable from its own
 * completion callback, which means the callback must not run before the
 * constructor has returned it. */
typedef struct
{
  ComfyTaskScope *operation;
  gboolean finished;
  gpointer value;
  char *path;
  GError *error;
} DeferredWait;

static void
deferred_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  DeferredWait *wait = user_data;

  wait->finished = TRUE;
  wait->value = g_task_propagate_pointer (G_TASK (result), &wait->error);
  wait->path = wait->value;
  comfy_task_scope_unref (wait->operation);
  wait->operation = NULL;
}

static void
test_immediate_failure_completes_after_the_constructor (void)
{
  DeferredWait wait = { 0 };

  wait.operation = comfy_task_scope_new (NULL, NULL, deferred_done, &wait);
  comfy_task_scope_return_error_deferred (
    wait.operation,
    g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "nothing to do"));

  g_assert_false (wait.finished);
  g_assert_nonnull (wait.operation);

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_true (wait.finished);
  g_assert_null (wait.value);
  g_assert_error (wait.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&wait.error);

  /* The pointer and boolean shapes carry the same guarantee: the answer
   * arrives with its value, and it arrives after the constructor. */
  wait.finished = FALSE;
  wait.value = NULL;
  wait.operation = comfy_task_scope_new (NULL, NULL, deferred_done, &wait);
  comfy_task_scope_return_pointer_deferred (wait.operation,
                                          g_strdup ("/a/path"), g_free);
  g_assert_false (wait.finished);

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_true (wait.finished);
  g_assert_no_error (wait.error);
  g_assert_cmpstr (wait.path, ==, "/a/path");
  g_clear_pointer (&wait.path, g_free);
  wait.value = NULL;
  g_print ("ok - an operation that finishes at once still returns its handle\n");
}

/* A cancel button that belongs to a row the caller does not own still has
 * to stop the operation. */
typedef struct
{
  ComfyTaskScope *operation;
  gboolean finished;
  gboolean cancelled;
  GError *error;
} FollowedWait;

static void
followed_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  FollowedWait *wait = user_data;

  wait->finished = TRUE;
  g_task_propagate_boolean (G_TASK (result), &wait->error);
  comfy_task_scope_unref (wait->operation);
  wait->operation = NULL;
}

/* What the work in flight does when it notices the cancellation. */
static gboolean
followed_report_cancelled (gpointer user_data)
{
  FollowedWait *wait = user_data;

  wait->cancelled = comfy_task_scope_return_error_if_cancelled (wait->operation);
  return G_SOURCE_REMOVE;
}

static void
test_scope_follows_a_cancellable_it_does_not_own (void)
{
  FollowedWait wait = { 0 };
  g_autoptr (GCancellable) row = g_cancellable_new ();
  g_autoptr (GCancellable) own = NULL;
  guint before = criticals;

  wait.operation = comfy_task_scope_new (NULL, NULL, followed_done, &wait);
  own = g_object_ref (comfy_task_scope_get_cancellable (wait.operation));
  comfy_task_scope_follow_cancellable (wait.operation, row);

  g_assert_false (g_cancellable_is_cancelled (own));
  g_cancellable_cancel (row);
  g_assert_true (g_cancellable_is_cancelled (own));

  comfy_task_scope_track_source (
    wait.operation, g_idle_add (followed_report_cancelled, &wait));

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_true (wait.finished);
  g_assert_true (wait.cancelled);
  g_assert_error (wait.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&wait.error);
  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - an operation follows a cancellable owned by its caller\n");
}

/* The shape that broke a modpack download: the operation is completed, and
 * the code that completed it then frees state that borrows the scope.
 * That state has to find the scope still there. */
typedef struct
{
  ComfyTaskScope *scope;
  gboolean answered;
} BorrowedState;

static void
borrowed_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  (void) user_data;
  g_task_propagate_boolean (G_TASK (result), NULL);
}

static void
borrowed_state_free (BorrowedState *state)
{
  /* This is what every request cleanup does: it takes the scope's sources
     away. It must run against a scope that is still alive. */
  if (state->scope != NULL)
    comfy_task_scope_remove_source (state->scope, 0);

  g_free (state);
}

static void
test_a_released_operation_outlives_the_code_that_completed_it (void)
{
  BorrowedState *state = g_new0 (BorrowedState, 1);
  guint before = criticals;

  state->scope = comfy_task_scope_unref_when_done (
    comfy_task_scope_new (NULL, NULL, borrowed_done, NULL));

  /* The completion runs from inside this call, as it does for an
     operation that finds out it has nothing to do. */
  comfy_task_scope_return_boolean (state->scope, TRUE);

  borrowed_state_free (state);

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - a released operation outlives the code that completed it\n");
}

/* Public callers commonly release their owned handle from the completion
 * callback. The scope must therefore remain alive until the whole
 * g_task_return_* call has returned, not merely until the callback starts. */
typedef struct
{
  ComfyTaskScope *scope;
} CallbackRelease;

static void
callback_releases_operation (GObject *source G_GNUC_UNUSED,
                             GAsyncResult *result,
                             gpointer user_data)
{
  CallbackRelease *release = user_data;

  g_task_propagate_boolean (G_TASK (result), NULL);
  comfy_task_scope_unref (release->scope);
  release->scope = NULL;
}

static void
test_callback_can_release_owned_operation (void)
{
  CallbackRelease release = { NULL };
  guint before = criticals;

  release.scope = comfy_task_scope_new (NULL, NULL,
                                      callback_releases_operation, &release);
  comfy_task_scope_return_boolean (release.scope, TRUE);

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_assert_null (release.scope);
  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - the completion callback can release its operation\n");
}

static void
test_scope_does_not_follow_its_own_cancellable (void)
{
  g_autoptr (ComfyTaskScope) scope = comfy_task_scope_new (NULL, NULL, NULL, NULL);
  guint before = criticals;

  comfy_task_scope_follow_cancellable (scope,
                                    comfy_task_scope_get_cancellable (scope));
  comfy_task_scope_cancel (scope);

  g_assert_cmpuint (criticals, ==, before);
  g_print ("ok - a scope does not recursively follow itself\n");
}

/* A step that starts nothing and finishes nothing, so the test decides
 * when and how the sequence completes. */
static void
manual_step (ComfyTaskSequence *sequence G_GNUC_UNUSED, gpointer user_data)
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
  ComfyTaskSequence *sequence;
  g_autofree char *first = g_strdup ("the one that counts");

  result.loop = g_main_loop_new (NULL, FALSE);

  sequence = comfy_task_sequence_new (NULL, sequence_done, &result, manual_step,
                                    &steps, NULL);
  comfy_task_sequence_ref (sequence);
  comfy_task_sequence_start (sequence);
  comfy_task_sequence_unref (sequence);
  g_assert_cmpuint (steps, ==, 1);

  comfy_task_sequence_return_pointer (sequence, g_steal_pointer (&first),
                                    g_free);

  /* Every way a late completion can arrive, each of which has to stop
     rather than act on state that is gone. The results handed to the
     ones that stop have to be released, which is what the result_free
     and the error are for. */
  comfy_task_sequence_return_pointer (sequence, g_strdup ("again"), g_free);
  comfy_task_sequence_return_error (sequence,
                                  g_error_new_literal (G_IO_ERROR,
                                                       G_IO_ERROR_FAILED,
                                                       "again"));
  comfy_task_sequence_next (sequence);

  g_main_loop_run (result.loop);

  /* The caller-owned reference keeps a late completion safe after the
   * result callback has released the task's reference. */
  comfy_task_sequence_return_pointer (sequence, g_strdup ("late"), g_free);
  comfy_task_sequence_unref (sequence);

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
  test_scope_reference_keeps_operation_alive ();
  test_immediate_failure_completes_after_the_constructor ();
  test_scope_follows_a_cancellable_it_does_not_own ();
  test_a_released_operation_outlives_the_code_that_completed_it ();
  test_callback_can_release_owned_operation ();
  test_scope_does_not_follow_its_own_cancellable ();
  test_scope_ids_and_states ();

  g_print ("# Task scope and sources it has already lost\n");
  test_scope_free_releases_a_source_that_fired ();
  test_scope_cancel_releases_a_source_that_fired ();
  test_scope_removes_a_source_that_fired ();
  test_scope_releases_mixed_sources ();
  return 0;
}
