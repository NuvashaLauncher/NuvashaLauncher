/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-task.h"

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
aml_task_sequence_get_task (AmlTaskSequence *sequence)
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
