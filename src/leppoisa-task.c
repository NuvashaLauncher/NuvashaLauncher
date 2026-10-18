/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-task-private.h"

/* A cancellable the scope watches without owning it, so that a UI whose
 * cancel button lives on someone else's object still stops the work. The
 * handler borrows the scope and never outlives it: unref() disconnects
 * every entry before the scope goes away, and the handler holds no
 * reference, so the pair cannot form a cycle. */
typedef struct
{
  GCancellable *cancellable;
  gulong handler;
} FollowedCancellable;

struct _LeppoisaTaskScope
{
  gatomicrefcount refs;
  guint64 id;
  LeppoisaTaskState state;
  GTask *task;
  GArray *sources;
  GArray *completions;
  GArray *followed;
  gboolean owns_task;
  gboolean unref_when_done;
};

static gint next_task_id;

static LeppoisaTaskScope *
leppoisa_task_scope_alloc (void)
{
  LeppoisaTaskScope *scope = g_new0 (LeppoisaTaskScope, 1);

  g_atomic_ref_count_init (&scope->refs);
  scope->id = (guint64) g_atomic_int_add (&next_task_id, 1) + 1;
  scope->state = leppoisa_TASK_STATE_QUEUED;
  scope->sources = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->completions = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->followed = g_array_new (FALSE, FALSE, sizeof (FollowedCancellable));

  return scope;
}

guint64
leppoisa_task_scope_get_id (const LeppoisaTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, 0);
  return scope->id;
}

LeppoisaTaskState
leppoisa_task_scope_get_state (const LeppoisaTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, leppoisa_TASK_STATE_FAILED);
  return scope->state;
}

void
leppoisa_task_scope_set_state (LeppoisaTaskScope *scope, LeppoisaTaskState state)
{
  g_return_if_fail (scope != NULL);
  if (scope->state != state)
    g_debug ("operation=%" G_GUINT64_FORMAT " state=%d->%d", scope->id,
             scope->state, state);
  scope->state = state;
}

static void
leppoisa_task_scope_unfollow (LeppoisaTaskScope *scope)
{
  for (guint i = 0; i < scope->followed->len; i++)
    {
      FollowedCancellable *followed =
        &g_array_index (scope->followed, FollowedCancellable, i);

      if (followed->handler != 0)
        g_cancellable_disconnect (followed->cancellable, followed->handler);
      g_clear_object (&followed->cancellable);
    }
  g_array_set_size (scope->followed, 0);
}

LeppoisaTaskScope *
leppoisa_task_scope_new (GObject *source_object, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GCancellable) owned = NULL;
  LeppoisaTaskScope *scope = leppoisa_task_scope_alloc ();

  /* A caller that owns the operation handle has to be able to stop it, so
   * an operation with no cancellable of its own is given one here rather
   * than leaving leppoisa_task_scope_cancel() with nothing to cancel. */
  if (cancellable == NULL)
    owned = g_cancellable_new ();

  scope->task = g_task_new (source_object, cancellable != NULL ? cancellable
                                                              : owned,
                            callback, user_data);
  scope->owns_task = TRUE;
  return scope;
}

LeppoisaTaskScope *
leppoisa_task_scope_from_task (GTask *task)
{
  LeppoisaTaskScope *scope = leppoisa_task_scope_alloc ();

  scope->task = task;
  return scope;
}

LeppoisaTaskScope *
leppoisa_task_scope_ref (LeppoisaTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  g_atomic_ref_count_inc (&scope->refs);
  return scope;
}

void
leppoisa_task_scope_unref (LeppoisaTaskScope *scope)
{
  if (scope == NULL || !g_atomic_ref_count_dec (&scope->refs))
    return;

  /* The task goes first: it is what the scope's borrowers are attached
     to, and dropping it runs their cleanups while this scope is still
     whole enough to answer them. */
  if (scope->owns_task)
    g_clear_object (&scope->task);

  for (guint i = 0; i < scope->sources->len; i++)
    {
      guint source_id = g_array_index (scope->sources, guint, i);
      if (g_main_context_find_source_by_id (NULL, source_id) != NULL)
        g_source_remove (source_id);
    }
  for (guint i = 0; i < scope->completions->len; i++)
    {
      guint source_id = g_array_index (scope->completions, guint, i);
      if (g_main_context_find_source_by_id (NULL, source_id) != NULL)
        g_source_remove (source_id);
    }
  g_array_unref (scope->sources);
  g_array_unref (scope->completions);
  leppoisa_task_scope_unfollow (scope);
  g_array_unref (scope->followed);
  g_free (scope);
}

void
leppoisa_task_scope_attach_task (GTask *task)
{
  g_return_if_fail (G_IS_TASK (task));

  g_object_set_data_full (G_OBJECT (task), "leppoisa-task-scope",
                          leppoisa_task_scope_from_task (task),
                          (GDestroyNotify) leppoisa_task_scope_unref);
}

GTask *
leppoisa_task_scope_task (const LeppoisaTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  return scope->task;
}

GCancellable *
leppoisa_task_scope_get_cancellable (const LeppoisaTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  return g_task_get_cancellable (scope->task);
}

void
leppoisa_task_scope_set_check_cancellable (LeppoisaTaskScope *scope, gboolean check)
{
  g_return_if_fail (scope != NULL);
  g_task_set_check_cancellable (scope->task, check);
}

/* A completion hands the operation to the caller's callback, and GTask
 * gives up its own reference before returning from that call. Nothing may
 * be freed underneath code that is still running there: the code that
 * completed the operation usually frees state that borrows its scope
 * straight afterwards. Releasing from the next main-loop turn is what
 * keeps that state looking at a scope that is still there.
 *
 * The reference the caller handed back is the one the idle owns: handing a
 * reference back means giving it away, not taking another one. */
static gboolean
release_when_done_idle (gpointer user_data)
{
  leppoisa_task_scope_unref (user_data);
  return G_SOURCE_REMOVE;
}

static void
scope_release_when_done (LeppoisaTaskScope *scope)
{
  scope->unref_when_done = FALSE;
  g_idle_add_full (G_PRIORITY_DEFAULT, release_when_done_idle, scope, NULL);
}

gboolean
leppoisa_task_scope_return_error_if_cancelled (LeppoisaTaskScope *scope)
{
  gboolean cancelled;
  LeppoisaTaskScope *hold;

  g_return_val_if_fail (scope != NULL, FALSE);

  hold = leppoisa_task_scope_ref (scope);
  cancelled = g_task_return_error_if_cancelled (scope->task);
  if (cancelled)
    leppoisa_task_scope_set_state (scope, leppoisa_TASK_STATE_CANCELLED);
  if (cancelled && scope->unref_when_done)
    scope_release_when_done (scope);

  leppoisa_task_scope_unref (hold);
  return cancelled;
}

void
leppoisa_task_scope_return_pointer (LeppoisaTaskScope *scope, gpointer result,
                               GDestroyNotify result_free)
{
  gboolean release;
  LeppoisaTaskScope *hold;

  g_return_if_fail (scope != NULL);
  leppoisa_task_scope_set_state (scope, leppoisa_TASK_STATE_COMPLETED);

  hold = leppoisa_task_scope_ref (scope);
  release = scope->unref_when_done;
  g_task_return_pointer (scope->task, result, result_free);
  if (release && scope->unref_when_done)
    scope_release_when_done (scope);
  leppoisa_task_scope_unref (hold);
}

void
leppoisa_task_scope_return_error (LeppoisaTaskScope *scope, GError *error)
{
  gboolean release;
  LeppoisaTaskScope *hold;

  g_return_if_fail (scope != NULL);
  leppoisa_task_scope_set_state (scope, leppoisa_TASK_STATE_FAILED);

  hold = leppoisa_task_scope_ref (scope);
  release = scope->unref_when_done;
  g_task_return_error (scope->task, error);
  if (release && scope->unref_when_done)
    scope_release_when_done (scope);
  leppoisa_task_scope_unref (hold);
}

void
leppoisa_task_scope_return_boolean (LeppoisaTaskScope *scope, gboolean result)
{
  gboolean release;
  LeppoisaTaskScope *hold;

  g_return_if_fail (scope != NULL);
  leppoisa_task_scope_set_state (scope, result ? leppoisa_TASK_STATE_COMPLETED
                                          : leppoisa_TASK_STATE_FAILED);

  hold = leppoisa_task_scope_ref (scope);
  release = scope->unref_when_done;
  g_task_return_boolean (scope->task, result);
  if (release && scope->unref_when_done)
    scope_release_when_done (scope);
  leppoisa_task_scope_unref (hold);
}

LeppoisaTaskScope *
leppoisa_task_scope_unref_when_done (LeppoisaTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  scope->unref_when_done = TRUE;
  return scope;
}

void
leppoisa_task_scope_release_when_done (LeppoisaTaskScope *scope, GCancellable *cancellable)
{
  leppoisa_task_scope_follow_cancellable (scope, cancellable);
  leppoisa_task_scope_unref_when_done (scope);
}

/* What an operation that has nothing to wait for has to hand over. The
 * scope is borrowed: the idle below is tracked by the scope, so the scope
 * removes it before the scope can go away and the pointer stays valid. */
typedef enum
{
  DEFERRED_POINTER,
  DEFERRED_ERROR,
  DEFERRED_BOOLEAN
} DeferredKind;

typedef struct
{
  LeppoisaTaskScope *scope;
  GTask *task;
  DeferredKind kind;
  gpointer value;
  GDestroyNotify value_free;
  GError *error;
  gboolean result;
} DeferredCompletion;

static void
deferred_completion_free (gpointer user_data)
{
  DeferredCompletion *deferred = user_data;

  /* A completion that was removed before it ran never handed its value to
     the task, so the value is released here. */
  if (deferred->kind == DEFERRED_POINTER)
    {
      if (deferred->value_free != NULL)
        deferred->value_free (deferred->value);
      else
        g_free (deferred->value);
    }

  g_clear_error (&deferred->error);
  g_clear_object (&deferred->task);
  leppoisa_task_scope_unref (deferred->scope);
  g_free (deferred);
}

static gboolean
deferred_completion_return (gpointer user_data)
{
  DeferredCompletion *deferred = user_data;

  /* Cancelling does not drop an answer that is already on its way: the
     operation reports the cancellation instead, so whoever started it is
     still answered and can release the handle it was given. */
  if (g_cancellable_is_cancelled (leppoisa_task_scope_get_cancellable (deferred->scope)))
    {
      leppoisa_task_scope_return_error (
        deferred->scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "the operation was cancelled"));
      return G_SOURCE_REMOVE;
    }

  switch (deferred->kind)
    {
    case DEFERRED_POINTER:
      leppoisa_task_scope_return_pointer (deferred->scope, deferred->value,
                                     deferred->value_free);
      deferred->value = NULL;
      deferred->value_free = NULL;
      break;
    case DEFERRED_ERROR:
      leppoisa_task_scope_return_error (deferred->scope,
                                   g_steal_pointer (&deferred->error));
      break;
    case DEFERRED_BOOLEAN:
      leppoisa_task_scope_return_boolean (deferred->scope, deferred->result);
      break;
    }

  return G_SOURCE_REMOVE;
}

static void
defer_completion (LeppoisaTaskScope *scope, DeferredCompletion *deferred)
{
  guint source_id;

  g_return_if_fail (scope != NULL);

  deferred->scope = leppoisa_task_scope_ref (scope);
  deferred->task = g_object_ref (scope->task);

  /* Tracked apart from the scope's own sources: cancelling must not take
     these away, only the end of the scope does. */
  source_id = g_idle_add_full (G_PRIORITY_DEFAULT, deferred_completion_return,
                              deferred, deferred_completion_free);
  if (source_id != 0)
    g_array_append_val (scope->completions, source_id);
}

void
leppoisa_task_scope_return_pointer_deferred (LeppoisaTaskScope *scope, gpointer result,
                                        GDestroyNotify result_free)
{
  g_return_if_fail (scope != NULL);

  DeferredCompletion *deferred = g_new0 (DeferredCompletion, 1);

  deferred->kind = DEFERRED_POINTER;
  deferred->value = result;
  deferred->value_free = result_free;
  defer_completion (scope, deferred);
}

void
leppoisa_task_scope_return_error_deferred (LeppoisaTaskScope *scope, GError *error)
{
  g_return_if_fail (scope != NULL);

  DeferredCompletion *deferred = g_new0 (DeferredCompletion, 1);

  deferred->kind = DEFERRED_ERROR;
  deferred->error = error;
  defer_completion (scope, deferred);
}

void
leppoisa_task_scope_return_boolean_deferred (LeppoisaTaskScope *scope, gboolean result)
{
  DeferredCompletion *deferred;

  g_return_if_fail (scope != NULL);

  deferred = g_new0 (DeferredCompletion, 1);
  deferred->kind = DEFERRED_BOOLEAN;
  deferred->result = result;
  defer_completion (scope, deferred);
}

void
leppoisa_task_scope_track_source (LeppoisaTaskScope *scope, guint source_id)
{
  g_return_if_fail (scope != NULL);

  if (source_id == 0)
    return;

  /* Registered once. The same source twice would be removed twice, and the
     second removal would find whatever has been given that id since. */
  for (guint i = 0; i < scope->sources->len; i++)
    if (g_array_index (scope->sources, guint, i) == source_id)
      return;

  g_array_append_val (scope->sources, source_id);
}

void
leppoisa_task_scope_remove_source (LeppoisaTaskScope *scope, guint source_id)
{
  g_return_if_fail (scope != NULL);
  for (guint i = 0; i < scope->sources->len; i++)
    if (g_array_index (scope->sources, guint, i) == source_id)
      {
        if (g_main_context_find_source_by_id (NULL, source_id) != NULL)
          g_source_remove (source_id);
        g_array_remove_index_fast (scope->sources, i);
        return;
      }
}

void
leppoisa_task_scope_forget_source (LeppoisaTaskScope *scope, guint source_id)
{
  g_return_if_fail (scope != NULL);
  for (guint i = 0; i < scope->sources->len; i++)
    if (g_array_index (scope->sources, guint, i) == source_id)
      {
        g_array_remove_index_fast (scope->sources, i);
        return;
      }
}

void
leppoisa_task_scope_cancel (LeppoisaTaskScope *scope)
{
  g_return_if_fail (scope != NULL);
  leppoisa_task_scope_set_state (scope, leppoisa_TASK_STATE_CANCELLED);
  GCancellable *cancellable = g_task_get_cancellable (scope->task);
  if (cancellable != NULL)
    g_cancellable_cancel (cancellable);
  for (guint i = 0; i < scope->sources->len; i++)
    {
      guint source_id = g_array_index (scope->sources, guint, i);
      if (g_main_context_find_source_by_id (NULL, source_id) != NULL)
        g_source_remove (source_id);
    }
  g_array_set_size (scope->sources, 0);
}

static void
on_followed_cancellable_cancelled (GCancellable *cancellable G_GNUC_UNUSED,
                                   gpointer      user_data)
{
  leppoisa_task_scope_cancel (user_data);
}

void
leppoisa_task_scope_follow_cancellable (LeppoisaTaskScope *scope, GCancellable *cancellable)
{
  FollowedCancellable followed;

  g_return_if_fail (scope != NULL);

  /* No cancellable is nothing to follow: the operation is simply not
     tied to anything else. */
  if (cancellable == NULL)
    return;

  /* Following the operation's own cancellable would call
     leppoisa_task_scope_cancel() recursively when that cancellable is cancelled.
     Only a cancellable owned by some other object belongs here. */
  if (cancellable == g_task_get_cancellable (scope->task))
    return;

  followed.cancellable = g_object_ref (cancellable);
  followed.handler =
    g_cancellable_connect (cancellable,
                           G_CALLBACK ((void (*) (void))
                                        on_followed_cancellable_cancelled),
                           scope, NULL);
  g_array_append_val (scope->followed, followed);
}

/* ---------------------------------------------------------- *
 * Watching a cancellable
 * ---------------------------------------------------------- */

/* The state behind leppoisa_cancellable_watch(). It lives as long as the source
   does, and the source's destroy notify is what tears it down. */
typedef struct
{
  GCancellable *cancellable;
  GSourceFunc callback;
  gpointer user_data;
  GDestroyNotify notify;
  gulong handler;
} CancellableWatch;

/* Answers from a source dispatch, which is a main-loop turn of its own. By
   then the cancellable's emission has returned, so the disconnect below is
   not one made from inside that emission, which GLib deadlocks on. */
static gboolean
watch_dispatch (gpointer user_data)
{
  CancellableWatch *watch = user_data;

  return watch->callback (watch->user_data);
}

static void
watch_free (gpointer user_data)
{
  CancellableWatch *watch = user_data;

  if (watch->handler != 0)
    g_cancellable_disconnect (watch->cancellable, watch->handler);
  g_clear_object (&watch->cancellable);

  if (watch->notify != NULL)
    watch->notify (watch->user_data);
  g_free (watch);
}

/* Marks the watch ready. Connecting to a cancellable that is already
   cancelled runs this before leppoisa_cancellable_watch() has attached the
   source, which is harmless: the source is not due until it is told. */
static void
watch_cancelled (GCancellable *cancellable G_GNUC_UNUSED,
                 gpointer      user_data)
{
  g_source_set_ready_time (user_data, 0);
}

guint
leppoisa_cancellable_watch (GCancellable  *cancellable,
                       GSourceFunc    callback,
                       gpointer       user_data,
                       GDestroyNotify notify)
{
  CancellableWatch *watch = g_new0 (CancellableWatch, 1);
  GSource *source;
  guint source_id;

  g_return_val_if_fail (G_IS_CANCELLABLE (cancellable), 0);
  g_return_val_if_fail (callback != NULL, 0);

  watch->callback = callback;
  watch->user_data = user_data;
  watch->notify = notify;
  watch->cancellable = g_object_ref (cancellable);

  /* A timeout that is due at a time that has not come: GLib compares a
     plain source's ready time against the clock, so this is how a source is
     written that stays quiet until it is told to speak. An idle source
     would not do - it ignores the ready time and dispatches on the first
     turn of the loop, cancelled or not. */
  source = g_timeout_source_new (0);
  g_source_set_ready_time (source, G_MAXINT64);
  g_source_set_callback (source, watch_dispatch, watch, watch_free);
  watch->handler =
    g_cancellable_connect (cancellable, G_CALLBACK (watch_cancelled), source,
                           NULL);

  source_id = g_source_attach (source, NULL);
  g_source_unref (source);

  return source_id;
}

struct _LeppoisaTaskSequence
{
  gatomicrefcount refs;
  GTask *task;
  LeppoisaTaskSequenceStep step;
  gpointer user_data;
  GDestroyNotify user_data_free;
  gboolean finished;
};

static void
sequence_finalize (gpointer user_data)
{
  leppoisa_task_sequence_unref (user_data);
}

/* Lets go of the step state and finishes the sequence. The struct itself
 * is not freed here: it is handed to the task it was built on, which
 * frees it when the task is finalized.
 *
 * The reason is the finished flag. A caller keeps the sequence pointer
 * after the sequence completes, and a late second completion reads that
 * flag to see that there is nothing left to do. Freeing the struct at
 * the moment of completion makes that read a read of freed memory, so
 * the flag can never be true and the protection it is written to give is
 * not there at all. Tying the struct to the task keeps it readable for
 * as long as anyone can still hold the pointer, because a caller that
 * holds the pointer also holds the task. */
static void
sequence_finish (LeppoisaTaskSequence *sequence)
{
  if (sequence->user_data_free != NULL)
    {
      sequence->user_data_free (sequence->user_data);
      sequence->user_data_free = NULL;
      sequence->user_data = NULL;
    }

  /* Transfer the sequence's internal lifetime reference to the task. */
  g_task_set_task_data (sequence->task, sequence, sequence_finalize);

  /* The task goes out to whoever completes it, and the sequence rides
     along with it, so the reference taken in leppoisa_task_sequence_new is
     given back here. A completed sequence has no task left to hand
     out, which is why the finished flag is checked before that. */
  g_clear_object (&sequence->task);
}

LeppoisaTaskSequence *
leppoisa_task_sequence_ref (LeppoisaTaskSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, NULL);
  g_atomic_ref_count_inc (&sequence->refs);
  return sequence;
}

void
leppoisa_task_sequence_unref (LeppoisaTaskSequence *sequence)
{
  if (sequence == NULL || !g_atomic_ref_count_dec (&sequence->refs))
    return;

  g_free (sequence);
}

LeppoisaTaskSequence *
leppoisa_task_sequence_new (GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer callback_data,
                       LeppoisaTaskSequenceStep step,
                       gpointer user_data,
                       GDestroyNotify user_data_free)
{
  LeppoisaTaskSequence *sequence = g_new0 (LeppoisaTaskSequence, 1);

  g_atomic_ref_count_init (&sequence->refs);
  /* One reference belongs to the caller; the other lasts until completion
   * transfers it to the GTask's task data. */
  g_atomic_ref_count_inc (&sequence->refs);
  sequence->task = g_task_new (NULL, cancellable, callback, callback_data);
  sequence->step = step;
  sequence->user_data = user_data;
  sequence->user_data_free = user_data_free;
  return sequence;
}

GCancellable *
leppoisa_task_sequence_get_cancellable (const LeppoisaTaskSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, NULL);
  return g_task_get_cancellable (sequence->task);
}

LeppoisaTaskScope *
leppoisa_task_sequence_scope (LeppoisaTaskSequence *sequence)
{
  LeppoisaTaskScope *scope;

  g_return_val_if_fail (sequence != NULL, NULL);

  /* A scope that takes over a reference of its own: the sequence keeps
     the task running regardless, and this is the reference that lets the
     handle outlive the caller that stored it. */
  scope = leppoisa_task_scope_from_task (g_object_ref (sequence->task));
  scope->owns_task = TRUE;
  return scope;
}

void
leppoisa_task_sequence_start (LeppoisaTaskSequence *sequence)
{
  g_return_if_fail (sequence != NULL);
  g_return_if_fail (sequence->step != NULL);

  if (g_task_return_error_if_cancelled (sequence->task))
    {
      sequence->finished = TRUE;
      sequence_finish (sequence);
      return;
    }

  sequence->step (sequence, sequence->user_data);
}

void
leppoisa_task_sequence_next (LeppoisaTaskSequence *sequence)
{
  g_return_if_fail (sequence != NULL);

  if (sequence->finished)
    return;

  if (g_task_return_error_if_cancelled (sequence->task))
    {
      sequence->finished = TRUE;
      sequence_finish (sequence);
      return;
    }

  sequence->step (sequence, sequence->user_data);
}

void
leppoisa_task_sequence_return_pointer (LeppoisaTaskSequence *sequence,
                                  gpointer result,
                                  GDestroyNotify result_free)
{
  g_return_if_fail (sequence != NULL);

  if (sequence->finished)
    {
      if (result_free != NULL)
        result_free (result);
      return;
    }

  sequence->finished = TRUE;
  g_task_return_pointer (sequence->task, result, result_free);
  sequence_finish (sequence);
}

void
leppoisa_task_sequence_return_error (LeppoisaTaskSequence *sequence, GError *error)
{
  g_return_if_fail (sequence != NULL);

  if (sequence->finished)
    {
      g_error_free (error);
      return;
    }

  sequence->finished = TRUE;
  g_task_return_error (sequence->task, error);
  sequence_finish (sequence);
}
