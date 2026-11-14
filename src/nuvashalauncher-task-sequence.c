/* Task sequence implementation. */

#include "nuvashalauncher-task-private.h"

struct _NuvashaLauncherTaskSequence
{
  gatomicrefcount refs;
  GTask *task;
  NuvashaLauncherTaskSequenceStep step;
  gpointer user_data;
  GDestroyNotify user_data_free;
  gboolean finished;
};

static void
sequence_finalize (gpointer user_data)
{
  nuvashalauncher_task_sequence_unref (user_data);
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
sequence_finish (NuvashaLauncherTaskSequence *sequence)
{
  if (sequence->user_data_free != NULL)
    {
      sequence->user_data_free (sequence->user_data);
      sequence->user_data_free = NULL;
      sequence->user_data = NULL;
    }

  /* Transfer the sequence's internal lifetime reference to the task. */
  g_task_set_task_data (sequence->task, sequence, sequence_finalize);
  nuvashalauncher_task_scope_release_attached (sequence->task);

  /* The task goes out to whoever completes it, and the sequence rides
     along with it, so the reference taken in nuvashalauncher_task_sequence_new is
     given back here. A completed sequence has no task left to hand
     out, which is why the finished flag is checked before that. */
  g_clear_object (&sequence->task);
}

NuvashaLauncherTaskSequence *
nuvashalauncher_task_sequence_ref (NuvashaLauncherTaskSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, NULL);
  g_atomic_ref_count_inc (&sequence->refs);
  return sequence;
}

void
nuvashalauncher_task_sequence_unref (NuvashaLauncherTaskSequence *sequence)
{
  if (sequence == NULL || !g_atomic_ref_count_dec (&sequence->refs))
    return;

  if (sequence->user_data_free != NULL)
    {
      sequence->user_data_free (sequence->user_data);
      sequence->user_data_free = NULL;
      sequence->user_data = NULL;
    }
  g_clear_object (&sequence->task);
  g_free (sequence);
}

NuvashaLauncherTaskSequence *
nuvashalauncher_task_sequence_new (GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer callback_data,
                       NuvashaLauncherTaskSequenceStep step,
                       gpointer user_data,
                       GDestroyNotify user_data_free)
{
  NuvashaLauncherTaskSequence *sequence = g_new0 (NuvashaLauncherTaskSequence, 1);

  g_atomic_ref_count_init (&sequence->refs);
  /* One reference belongs to the caller; the other lasts until completion
   * transfers it to the GTask's task data. */
  g_atomic_ref_count_inc (&sequence->refs);
  sequence->task = g_task_new (NULL, cancellable, callback, callback_data);
  g_task_set_source_tag (sequence->task, "nuvashalauncher-task-sequence");
  nuvashalauncher_task_scope_attach_task (sequence->task);
  sequence->step = step;
  sequence->user_data = user_data;
  sequence->user_data_free = user_data_free;
  return sequence;
}

GCancellable *
nuvashalauncher_task_sequence_get_cancellable (const NuvashaLauncherTaskSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, NULL);
  return g_task_get_cancellable (sequence->task);
}

NuvashaLauncherTaskScope *
nuvashalauncher_task_sequence_scope (NuvashaLauncherTaskSequence *sequence)
{
  NuvashaLauncherTaskScope *scope;

  g_return_val_if_fail (sequence != NULL, NULL);
  if (sequence->task == NULL)
    return NULL;

  /* A scope that takes over a reference of its own: the sequence keeps
     the task running regardless, and this is the reference that lets the
     handle outlive the caller that stored it. */
  scope = g_object_get_data (G_OBJECT (sequence->task), "nuvashalauncher-task-scope");
  if (scope != NULL)
    return nuvashalauncher_task_scope_ref (scope);

  scope = nuvashalauncher_task_scope_from_task (g_object_ref (sequence->task));
  nuvashalauncher_task_scope_take_task (scope);
  return scope;
}

void
nuvashalauncher_task_sequence_start (NuvashaLauncherTaskSequence *sequence)
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
nuvashalauncher_task_sequence_next (NuvashaLauncherTaskSequence *sequence)
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
nuvashalauncher_task_sequence_return_pointer (NuvashaLauncherTaskSequence *sequence,
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
nuvashalauncher_task_sequence_return_error (NuvashaLauncherTaskSequence *sequence, GError *error)
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
