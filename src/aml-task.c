/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-task-private.h"

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

struct _AmlTaskScope
{
  gatomicrefcount refs;
  GTask *task;
  GArray *sources;
  GArray *completions;
  GArray *followed;
  gboolean owns_task;
  gboolean unref_when_done;
};

static AmlTaskScope *
aml_task_scope_alloc (void)
{
  AmlTaskScope *scope = g_new0 (AmlTaskScope, 1);

  g_atomic_ref_count_init (&scope->refs);
  scope->sources = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->completions = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->followed = g_array_new (FALSE, FALSE, sizeof (FollowedCancellable));

  return scope;
}

static void
aml_task_scope_unfollow (AmlTaskScope *scope)
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

AmlTaskScope *
aml_task_scope_new (GObject *source_object, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GCancellable) owned = NULL;
  AmlTaskScope *scope = aml_task_scope_alloc ();

  /* A caller that owns the operation handle has to be able to stop it, so
   * an operation with no cancellable of its own is given one here rather
   * than leaving aml_task_scope_cancel() with nothing to cancel. */
  if (cancellable == NULL)
    owned = g_cancellable_new ();

  scope->task = g_task_new (source_object, cancellable != NULL ? cancellable
                                                              : owned,
                            callback, user_data);
  scope->owns_task = TRUE;
  return scope;
}

AmlTaskScope *
aml_task_scope_from_task (GTask *task)
{
  AmlTaskScope *scope = aml_task_scope_alloc ();

  scope->task = task;
  return scope;
}

AmlTaskScope *
aml_task_scope_ref (AmlTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  g_atomic_ref_count_inc (&scope->refs);
  return scope;
}

void
aml_task_scope_unref (AmlTaskScope *scope)
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
  aml_task_scope_unfollow (scope);
  g_array_unref (scope->followed);
  g_free (scope);
}

void
aml_task_scope_attach_task (GTask *task)
{
  g_return_if_fail (G_IS_TASK (task));

  g_object_set_data_full (G_OBJECT (task), "aml-task-scope",
                          aml_task_scope_from_task (task),
                          (GDestroyNotify) aml_task_scope_unref);
}

GTask *
aml_task_scope_task (const AmlTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  return scope->task;
}

GCancellable *
aml_task_scope_get_cancellable (const AmlTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  return g_task_get_cancellable (scope->task);
}

void
aml_task_scope_set_check_cancellable (AmlTaskScope *scope, gboolean check)
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
  aml_task_scope_unref (user_data);
  return G_SOURCE_REMOVE;
}

static void
scope_release_when_done (AmlTaskScope *scope)
{
  scope->unref_when_done = FALSE;
  g_idle_add_full (G_PRIORITY_DEFAULT, release_when_done_idle, scope, NULL);
}

gboolean
aml_task_scope_return_error_if_cancelled (AmlTaskScope *scope)
{
  gboolean cancelled;

  g_return_val_if_fail (scope != NULL, FALSE);

  cancelled = g_task_return_error_if_cancelled (scope->task);
  if (cancelled && scope->unref_when_done)
    scope_release_when_done (scope);

  return cancelled;
}

void
aml_task_scope_return_pointer (AmlTaskScope *scope, gpointer result,
                               GDestroyNotify result_free)
{
  gboolean release;

  g_return_if_fail (scope != NULL);

  release = scope->unref_when_done;
  g_task_return_pointer (scope->task, result, result_free);
  if (release && scope->unref_when_done)
    scope_release_when_done (scope);
}

void
aml_task_scope_return_error (AmlTaskScope *scope, GError *error)
{
  gboolean release;

  g_return_if_fail (scope != NULL);

  release = scope->unref_when_done;
  g_task_return_error (scope->task, error);
  if (release && scope->unref_when_done)
    scope_release_when_done (scope);
}

void
aml_task_scope_return_boolean (AmlTaskScope *scope, gboolean result)
{
  gboolean release;

  g_return_if_fail (scope != NULL);

  release = scope->unref_when_done;
  g_task_return_boolean (scope->task, result);
  if (release && scope->unref_when_done)
    scope_release_when_done (scope);
}

AmlTaskScope *
aml_task_scope_unref_when_done (AmlTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  scope->unref_when_done = TRUE;
  return scope;
}

void
aml_task_scope_release_when_done (AmlTaskScope *scope, GCancellable *cancellable)
{
  aml_task_scope_follow_cancellable (scope, cancellable);
  aml_task_scope_unref_when_done (scope);
}

/* What an operation that has nothing to wait for has to hand over. The
 * scope is borrowed: the idle below is tracked by the scope, so the scope
 * removes it before the scope can go away and the pointer stays valid. */
typedef enum
{
  DEFERRED_POINTER,
  DEFERRED_BOOLEAN,
  DEFERRED_ERROR
} DeferredKind;

typedef struct
{
  AmlTaskScope *scope;
  DeferredKind kind;
  gpointer value;
  GDestroyNotify value_free;
  gboolean boolean;
  GError *error;
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
  g_free (deferred);
}

static gboolean
deferred_completion_return (gpointer user_data)
{
  DeferredCompletion *deferred = user_data;

  /* Cancelling does not drop an answer that is already on its way: the
     operation reports the cancellation instead, so whoever started it is
     still answered and can release the handle it was given. */
  if (g_cancellable_is_cancelled (aml_task_scope_get_cancellable (deferred->scope)))
    {
      aml_task_scope_return_error (
        deferred->scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "the operation was cancelled"));
      return G_SOURCE_REMOVE;
    }

  switch (deferred->kind)
    {
    case DEFERRED_POINTER:
      aml_task_scope_return_pointer (deferred->scope, deferred->value,
                                     deferred->value_free);
      deferred->value = NULL;
      deferred->value_free = NULL;
      break;
    case DEFERRED_BOOLEAN:
      aml_task_scope_return_boolean (deferred->scope, deferred->boolean);
      break;
    case DEFERRED_ERROR:
      aml_task_scope_return_error (deferred->scope,
                                   g_steal_pointer (&deferred->error));
      break;
    }

  return G_SOURCE_REMOVE;
}

static void
defer_completion (AmlTaskScope *scope, DeferredCompletion *deferred)
{
  guint source_id;

  deferred->scope = scope;

  /* Tracked apart from the scope's own sources: cancelling must not take
     these away, only the end of the scope does. */
  source_id = g_idle_add_full (G_PRIORITY_DEFAULT, deferred_completion_return,
                              deferred, deferred_completion_free);
  if (source_id != 0)
    g_array_append_val (scope->completions, source_id);
}

void
aml_task_scope_return_pointer_deferred (AmlTaskScope *scope, gpointer result,
                                        GDestroyNotify result_free)
{
  DeferredCompletion *deferred = g_new0 (DeferredCompletion, 1);

  g_return_if_fail (scope != NULL);

  deferred->kind = DEFERRED_POINTER;
  deferred->value = result;
  deferred->value_free = result_free;
  defer_completion (scope, deferred);
}

void
aml_task_scope_return_boolean_deferred (AmlTaskScope *scope, gboolean result)
{
  DeferredCompletion *deferred = g_new0 (DeferredCompletion, 1);

  g_return_if_fail (scope != NULL);

  deferred->kind = DEFERRED_BOOLEAN;
  deferred->boolean = result;
  defer_completion (scope, deferred);
}

void
aml_task_scope_return_error_deferred (AmlTaskScope *scope, GError *error)
{
  DeferredCompletion *deferred = g_new0 (DeferredCompletion, 1);

  g_return_if_fail (scope != NULL);

  deferred->kind = DEFERRED_ERROR;
  deferred->error = error;
  defer_completion (scope, deferred);
}

void
aml_task_scope_track_source (AmlTaskScope *scope, guint source_id)
{
  g_return_if_fail (scope != NULL);
  if (source_id != 0)
    g_array_append_val (scope->sources, source_id);
}

void
aml_task_scope_remove_source (AmlTaskScope *scope, guint source_id)
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
aml_task_scope_forget_source (AmlTaskScope *scope, guint source_id)
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
aml_task_scope_cancel (AmlTaskScope *scope)
{
  g_return_if_fail (scope != NULL);
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
  aml_task_scope_cancel (user_data);
}

void
aml_task_scope_follow_cancellable (AmlTaskScope *scope, GCancellable *cancellable)
{
  FollowedCancellable followed;

  g_return_if_fail (scope != NULL);

  /* No cancellable is nothing to follow: the operation is simply not
     tied to anything else. */
  if (cancellable == NULL)
    return;

  followed.cancellable = g_object_ref (cancellable);
  followed.handler =
    g_cancellable_connect (cancellable,
                           G_CALLBACK ((void (*) (void))
                                        on_followed_cancellable_cancelled),
                           scope, NULL);
  g_array_append_val (scope->followed, followed);
}

struct _AmlTaskSequence
{
  gatomicrefcount refs;
  GTask *task;
  AmlTaskSequenceStep step;
  gpointer user_data;
  GDestroyNotify user_data_free;
  gboolean finished;
};

static void
sequence_finalize (gpointer user_data)
{
  aml_task_sequence_unref (user_data);
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
sequence_finish (AmlTaskSequence *sequence)
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
     along with it, so the reference taken in aml_task_sequence_new is
     given back here. A completed sequence has no task left to hand
     out, which is why the finished flag is checked before that. */
  g_clear_object (&sequence->task);
}

AmlTaskSequence *
aml_task_sequence_ref (AmlTaskSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, NULL);
  g_atomic_ref_count_inc (&sequence->refs);
  return sequence;
}

void
aml_task_sequence_unref (AmlTaskSequence *sequence)
{
  if (sequence == NULL || !g_atomic_ref_count_dec (&sequence->refs))
    return;

  g_free (sequence);
}

AmlTaskSequence *
aml_task_sequence_new (GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer callback_data,
                       AmlTaskSequenceStep step,
                       gpointer user_data,
                       GDestroyNotify user_data_free)
{
  AmlTaskSequence *sequence = g_new0 (AmlTaskSequence, 1);

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
aml_task_sequence_get_cancellable (const AmlTaskSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, NULL);
  return g_task_get_cancellable (sequence->task);
}

AmlTaskScope *
aml_task_sequence_scope (AmlTaskSequence *sequence)
{
  AmlTaskScope *scope;

  g_return_val_if_fail (sequence != NULL, NULL);

  /* A scope that takes over a reference of its own: the sequence keeps
     the task running regardless, and this is the reference that lets the
     handle outlive the caller that stored it. */
  scope = aml_task_scope_from_task (g_object_ref (sequence->task));
  scope->owns_task = TRUE;
  return scope;
}

void
aml_task_sequence_start (AmlTaskSequence *sequence)
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
aml_task_sequence_next (AmlTaskSequence *sequence)
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
aml_task_sequence_return_pointer (AmlTaskSequence *sequence,
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
aml_task_sequence_return_error (AmlTaskSequence *sequence, GError *error)
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
