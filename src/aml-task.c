/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-task.h"

struct _AmlTaskScope
{
  gatomicrefcount refs;
  GTask *task;
  GArray *sources;
  gboolean owns_task;
};

AmlTaskScope *
aml_task_scope_new (GObject *source_object, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  AmlTaskScope *scope = g_new0 (AmlTaskScope, 1);
  g_atomic_ref_count_init (&scope->refs);
  scope->task = g_task_new (source_object, cancellable, callback, user_data);
  scope->sources = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->owns_task = TRUE;
  return scope;
}

AmlTaskScope *
aml_task_scope_from_task (GTask *task)
{
  AmlTaskScope *scope = g_new0 (AmlTaskScope, 1);
  g_atomic_ref_count_init (&scope->refs);
  scope->task = task;
  scope->sources = g_array_new (FALSE, FALSE, sizeof (guint));
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

  for (guint i = 0; i < scope->sources->len; i++)
    {
      guint source_id = g_array_index (scope->sources, guint, i);
      if (g_main_context_find_source_by_id (NULL, source_id) != NULL)
        g_source_remove (source_id);
    }
  g_array_unref (scope->sources);
  if (scope->owns_task)
    g_clear_object (&scope->task);
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
aml_task_scope_get_task (const AmlTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  return scope->task;
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

  sequence->step (sequence->task, sequence->user_data);
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

  sequence->step (sequence->task, sequence->user_data);
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
