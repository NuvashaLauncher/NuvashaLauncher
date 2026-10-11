/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-task.h"

struct _AmlTaskScope
{
  GTask *task;
  GArray *sources;
  gboolean owns_task;
};

AmlTaskScope *
aml_task_scope_new (GObject *source_object, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  AmlTaskScope *scope = g_new0 (AmlTaskScope, 1);
  scope->task = g_task_new (source_object, cancellable, callback, user_data);
  scope->sources = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->owns_task = TRUE;
  return scope;
}

AmlTaskScope *
aml_task_scope_borrow_task (GTask *task)
{
  AmlTaskScope *scope = g_new0 (AmlTaskScope, 1);
  scope->task = task;
  scope->sources = g_array_new (FALSE, FALSE, sizeof (guint));
  return scope;
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

void
aml_task_scope_free (AmlTaskScope *scope)
{
  if (scope == NULL)
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

struct _AmlTaskSequence
{
  GTask *task;
  AmlTaskSequenceStep step;
  gpointer user_data;
  GDestroyNotify user_data_free;
  gboolean finished;
};

static void
sequence_free (AmlTaskSequence *sequence)
{
  if (sequence->user_data_free != NULL)
    sequence->user_data_free (sequence->user_data);
  g_clear_object (&sequence->task);
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

  sequence->task = g_task_new (NULL, cancellable, callback, callback_data);
  sequence->step = step;
  sequence->user_data = user_data;
  sequence->user_data_free = user_data_free;
  return sequence;
}

GTask *
aml_task_sequence_get_task (const AmlTaskSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, NULL);
  return sequence->task;
}

void
aml_task_sequence_start (AmlTaskSequence *sequence)
{
  g_return_if_fail (sequence != NULL);
  g_return_if_fail (sequence->step != NULL);

  if (g_task_return_error_if_cancelled (sequence->task))
    {
      sequence->finished = TRUE;
      sequence_free (sequence);
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
      sequence_free (sequence);
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
  sequence_free (sequence);
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
  sequence_free (sequence);
}
