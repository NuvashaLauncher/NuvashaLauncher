/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-task-private.h"

/* A cancellable followed by a task scope without taking ownership. The
 * signal handler is disconnected before the scope is destroyed. */
typedef struct
{
  GCancellable *cancellable;
  gulong handler;
} FollowedCancellable;

struct _nuvashalauncherTaskScope
{
  gatomicrefcount refs;
  guint64 id;
  gint state;
  GTask *task;
  GMainContext *context;
  GArray *sources;
  GArray *completions;
  GArray *followed;
  gboolean owns_task;
  gint unref_when_done;
};

static gint next_task_id;

static nuvashalauncherTaskScope *
nuvashalauncher_task_scope_alloc (void)
{
  nuvashalauncherTaskScope *scope = g_new0 (nuvashalauncherTaskScope, 1);

  g_atomic_ref_count_init (&scope->refs);
  scope->id = (guint64) g_atomic_int_add (&next_task_id, 1) + 1;
  scope->state = nuvashalauncher_TASK_STATE_QUEUED;
  scope->sources = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->completions = g_array_new (FALSE, FALSE, sizeof (guint));
  scope->followed = g_array_new (FALSE, FALSE, sizeof (FollowedCancellable));

  return scope;
}

guint64
nuvashalauncher_task_scope_get_id (const nuvashalauncherTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, 0);
  return scope->id;
}

nuvashalauncherTaskState
nuvashalauncher_task_scope_get_state (const nuvashalauncherTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, nuvashalauncher_TASK_STATE_FAILED);
  return (nuvashalauncherTaskState) g_atomic_int_get (&scope->state);
}

void
nuvashalauncher_task_scope_set_source_tag (nuvashalauncherTaskScope *scope, gpointer source_tag)
{
  g_return_if_fail (scope != NULL);
  g_task_set_source_tag (scope->task, source_tag);
}

gboolean
nuvashalauncher_task_result_is_valid (GAsyncResult *result, const char *expected_tag)
{
  const char *tag;
  if (!G_IS_TASK (result))
    return FALSE;
  tag = g_task_get_source_tag (G_TASK (result));
  if (tag == NULL)
    return FALSE;

  if (expected_tag == NULL)
    return TRUE;

  if (g_str_equal (tag, expected_tag) ||
      g_str_equal (tag, "nuvashalauncher-task-sequence"))
    return TRUE;

  /* A generic scope result must actually have been created by this helper;
   * a caller-supplied GTask with an arbitrary diagnostic tag is not enough. */
  return g_str_equal (expected_tag, "nuvashalauncher-task-scope") &&
         g_object_get_data (G_OBJECT (result),
                            "nuvashalauncher-task-scope-marker") != NULL;
}

void
nuvashalauncher_task_scope_set_state (nuvashalauncherTaskScope *scope, nuvashalauncherTaskState state)
{
  g_return_if_fail (scope != NULL);
  nuvashalauncherTaskState previous =
    (nuvashalauncherTaskState) g_atomic_int_get (&scope->state);
  if (previous != state)
    g_debug ("operation=%" G_GUINT64_FORMAT " state=%d->%d", scope->id,
             previous, state);
  g_atomic_int_set (&scope->state, state);
}

static void
nuvashalauncher_task_scope_unfollow (nuvashalauncherTaskScope *scope)
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

nuvashalauncherTaskScope *
nuvashalauncher_task_scope_new_tagged (GObject *source_object, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data,
                               const char *source_tag)
{
  g_autoptr (GCancellable) owned = NULL;
  nuvashalauncherTaskScope *scope = nuvashalauncher_task_scope_alloc ();

  /* A caller that owns the operation handle has to be able to stop it, so
   * an operation with no cancellable of its own is given one here rather
   * than leaving nuvashalauncher_task_scope_cancel() with nothing to cancel. */
  if (cancellable == NULL)
    owned = g_cancellable_new ();

  scope->task = g_task_new (source_object, cancellable != NULL ? cancellable
                                                                : owned,
                             callback, user_data);
  scope->context = g_main_context_ref (g_task_get_context (scope->task));
  /* Every scope has a useful diagnostic tag, including legacy callers that
   * do not provide a more specific operation tag. Public operations may
   * replace this with their API-specific tag. */
  g_task_set_source_tag (scope->task,
                         (gpointer) (source_tag != NULL ? source_tag
                                                          : "nuvashalauncher-task-scope"));
  g_object_set_data (G_OBJECT (scope->task),
                     "nuvashalauncher-task-scope-marker", scope->task);
  nuvashalauncher_task_scope_take_task (scope);
  return scope;
}

nuvashalauncherTaskScope *
nuvashalauncher_task_scope_from_task (GTask *task)
{
  nuvashalauncherTaskScope *scope = nuvashalauncher_task_scope_alloc ();

  scope->task = task;
  scope->context = g_main_context_ref (g_task_get_context (task));
  return scope;
}

nuvashalauncherTaskScope *
nuvashalauncher_task_scope_ref (nuvashalauncherTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  g_atomic_ref_count_inc (&scope->refs);
  return scope;
}

void
nuvashalauncher_task_scope_unref (nuvashalauncherTaskScope *scope)
{
  if (scope == NULL || !g_atomic_ref_count_dec (&scope->refs))
    return;

  /* Drop the task first so borrower cleanup runs while the scope is valid. */
  if (scope->owns_task)
    g_clear_object (&scope->task);

  for (guint i = 0; i < scope->sources->len; i++)
    {
      guint source_id = g_array_index (scope->sources, guint, i);
       GSource *source = g_main_context_find_source_by_id (scope->context, source_id);
       if (source != NULL)
         g_source_destroy (source);
    }
  for (guint i = 0; i < scope->completions->len; i++)
    {
      guint source_id = g_array_index (scope->completions, guint, i);
       GSource *source = g_main_context_find_source_by_id (scope->context, source_id);
       if (source != NULL)
         g_source_destroy (source);
    }
  g_array_unref (scope->sources);
  g_array_unref (scope->completions);
  nuvashalauncher_task_scope_unfollow (scope);
  g_array_unref (scope->followed);
  g_main_context_unref (scope->context);
  g_free (scope);
}

void
nuvashalauncher_task_scope_attach_task (GTask *task)
{
  g_return_if_fail (G_IS_TASK (task));

  if (g_task_get_source_tag (task) == NULL)
    g_task_set_source_tag (task, "nuvashalauncher-task-scope");

  g_object_set_data (G_OBJECT (task),
                     "nuvashalauncher-task-scope-marker", task);

  g_object_set_data_full (G_OBJECT (task), "nuvashalauncher-task-scope",
                          nuvashalauncher_task_scope_from_task (task),
                          (GDestroyNotify) nuvashalauncher_task_scope_unref);
}

GTask *
nuvashalauncher_task_scope_task (const nuvashalauncherTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  return scope->task;
}

GCancellable *
nuvashalauncher_task_scope_get_cancellable (const nuvashalauncherTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  return g_task_get_cancellable (scope->task);
}

void
nuvashalauncher_task_scope_set_check_cancellable (nuvashalauncherTaskScope *scope, gboolean check)
{
  g_return_if_fail (scope != NULL);
  g_task_set_check_cancellable (scope->task, check);
}

/* Defer the caller's unref until the callback returns. This prevents callback
 * cleanup from destroying the scope while completion code still uses it. */
static gboolean
release_when_done_idle (gpointer user_data)
{
  nuvashalauncher_task_scope_unref (user_data);
  return G_SOURCE_REMOVE;
}

static void
scope_release_when_done (nuvashalauncherTaskScope *scope)
{
  g_atomic_int_set (&scope->unref_when_done, FALSE);
  GSource *source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, release_when_done_idle, scope, NULL);
  g_source_attach (source, scope->context);
  g_source_unref (source);
}

gboolean
nuvashalauncher_task_scope_return_error_if_cancelled (nuvashalauncherTaskScope *scope)
{
  gboolean cancelled;
  nuvashalauncherTaskScope *hold;

  g_return_val_if_fail (scope != NULL, FALSE);

  hold = nuvashalauncher_task_scope_ref (scope);
  cancelled = g_task_return_error_if_cancelled (scope->task);
  if (cancelled)
    nuvashalauncher_task_scope_set_state (scope, nuvashalauncher_TASK_STATE_CANCELLED);
  if (cancelled && g_atomic_int_get (&scope->unref_when_done))
    scope_release_when_done (scope);

  nuvashalauncher_task_scope_unref (hold);
  return cancelled;
}

void
nuvashalauncher_task_scope_return_pointer (nuvashalauncherTaskScope *scope, gpointer result,
                               GDestroyNotify result_free)
{
  gboolean release;
  nuvashalauncherTaskScope *hold;

  g_return_if_fail (scope != NULL);
  nuvashalauncher_task_scope_set_state (scope, nuvashalauncher_TASK_STATE_COMPLETED);

  hold = nuvashalauncher_task_scope_ref (scope);
  release = g_atomic_int_get (&scope->unref_when_done);
  g_task_return_pointer (scope->task, result, result_free);
  if (release && g_atomic_int_get (&scope->unref_when_done))
    scope_release_when_done (scope);
  nuvashalauncher_task_scope_unref (hold);
}

void
nuvashalauncher_task_scope_return_error (nuvashalauncherTaskScope *scope, GError *error)
{
  gboolean release;
  nuvashalauncherTaskScope *hold;

  g_return_if_fail (scope != NULL);
  nuvashalauncher_task_scope_set_state (scope, nuvashalauncher_TASK_STATE_FAILED);

  hold = nuvashalauncher_task_scope_ref (scope);
  release = g_atomic_int_get (&scope->unref_when_done);
  g_task_return_error (scope->task, error);
  if (release && g_atomic_int_get (&scope->unref_when_done))
    scope_release_when_done (scope);
  nuvashalauncher_task_scope_unref (hold);
}

void
nuvashalauncher_task_scope_return_boolean (nuvashalauncherTaskScope *scope, gboolean result)
{
  gboolean release;
  nuvashalauncherTaskScope *hold;

  g_return_if_fail (scope != NULL);
  nuvashalauncher_task_scope_set_state (scope, result ? nuvashalauncher_TASK_STATE_COMPLETED
                                          : nuvashalauncher_TASK_STATE_FAILED);

  hold = nuvashalauncher_task_scope_ref (scope);
  release = g_atomic_int_get (&scope->unref_when_done);
  g_task_return_boolean (scope->task, result);
  if (release && g_atomic_int_get (&scope->unref_when_done))
    scope_release_when_done (scope);
  nuvashalauncher_task_scope_unref (hold);
}

nuvashalauncherTaskScope *
nuvashalauncher_task_scope_unref_when_done (nuvashalauncherTaskScope *scope)
{
  g_return_val_if_fail (scope != NULL, NULL);
  g_atomic_int_set (&scope->unref_when_done, TRUE);
  return scope;
}

void
nuvashalauncher_task_scope_release_when_done (nuvashalauncherTaskScope *scope, GCancellable *cancellable)
{
  nuvashalauncher_task_scope_follow_cancellable (scope, cancellable);
  nuvashalauncher_task_scope_unref_when_done (scope);
}

/* Deferred completion for operations with no pending source. The scope tracks
 * the idle source and removes it before destruction. */
typedef enum
{
  DEFERRED_POINTER,
  DEFERRED_ERROR,
  DEFERRED_BOOLEAN
} DeferredKind;

typedef struct
{
  nuvashalauncherTaskScope *scope;
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
  nuvashalauncher_task_scope_unref (deferred->scope);
  g_free (deferred);
}

static gboolean
deferred_completion_return (gpointer user_data)
{
  DeferredCompletion *deferred = user_data;

  /* Cancelling does not drop an answer that is already on its way: the
     operation reports the cancellation instead, so whoever started it is
     still answered and can release the handle it was given. */
  if (g_cancellable_is_cancelled (nuvashalauncher_task_scope_get_cancellable (deferred->scope)))
    {
      nuvashalauncher_task_scope_return_error (
        deferred->scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "the operation was cancelled"));
      return G_SOURCE_REMOVE;
    }

  switch (deferred->kind)
    {
    case DEFERRED_POINTER:
      nuvashalauncher_task_scope_return_pointer (deferred->scope, deferred->value,
                                     deferred->value_free);
      deferred->value = NULL;
      deferred->value_free = NULL;
      break;
    case DEFERRED_ERROR:
      nuvashalauncher_task_scope_return_error (deferred->scope,
                                   g_steal_pointer (&deferred->error));
      break;
    case DEFERRED_BOOLEAN:
      nuvashalauncher_task_scope_return_boolean (deferred->scope, deferred->result);
      break;
    }

  return G_SOURCE_REMOVE;
}

static void
defer_completion (nuvashalauncherTaskScope *scope, DeferredCompletion *deferred)
{
  guint source_id;

  g_return_if_fail (scope != NULL);

  deferred->scope = nuvashalauncher_task_scope_ref (scope);
  deferred->task = g_object_ref (scope->task);

  GSource *source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, deferred_completion_return, deferred,
                         deferred_completion_free);
  source_id = g_source_attach (source, scope->context);
  g_source_unref (source);
  if (source_id != 0)
    g_array_append_val (scope->completions, source_id);
}

void
nuvashalauncher_task_scope_return_pointer_deferred (nuvashalauncherTaskScope *scope, gpointer result,
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
nuvashalauncher_task_scope_return_error_deferred (nuvashalauncherTaskScope *scope, GError *error)
{
  g_return_if_fail (scope != NULL);

  DeferredCompletion *deferred = g_new0 (DeferredCompletion, 1);

  deferred->kind = DEFERRED_ERROR;
  deferred->error = error;
  defer_completion (scope, deferred);
}

void
nuvashalauncher_task_scope_return_boolean_deferred (nuvashalauncherTaskScope *scope, gboolean result)
{
  DeferredCompletion *deferred;

  g_return_if_fail (scope != NULL);

  deferred = g_new0 (DeferredCompletion, 1);
  deferred->kind = DEFERRED_BOOLEAN;
  deferred->result = result;
  defer_completion (scope, deferred);
}

void
nuvashalauncher_task_scope_track_source (nuvashalauncherTaskScope *scope, guint source_id)
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
nuvashalauncher_task_scope_remove_source (nuvashalauncherTaskScope *scope, guint source_id)
{
  g_return_if_fail (scope != NULL);
  for (guint i = 0; i < scope->sources->len; i++)
    if (g_array_index (scope->sources, guint, i) == source_id)
      {
        GSource *source = g_main_context_find_source_by_id (scope->context, source_id);
        if (source != NULL)
          g_source_destroy (source);
        g_array_remove_index_fast (scope->sources, i);
        return;
      }
}

void
nuvashalauncher_task_scope_forget_source (nuvashalauncherTaskScope *scope, guint source_id)
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
nuvashalauncher_task_scope_cancel (nuvashalauncherTaskScope *scope)
{
  g_return_if_fail (scope != NULL);
  nuvashalauncher_task_scope_set_state (scope, nuvashalauncher_TASK_STATE_CANCELLED);
  GCancellable *cancellable = g_task_get_cancellable (scope->task);
  if (cancellable != NULL)
    g_cancellable_cancel (cancellable);
  for (guint i = 0; i < scope->sources->len; i++)
    {
      guint source_id = g_array_index (scope->sources, guint, i);
       GSource *source = g_main_context_find_source_by_id (scope->context, source_id);
       if (source != NULL)
         g_source_destroy (source);
    }
  g_array_set_size (scope->sources, 0);
}

static void
on_followed_cancellable_cancelled (GCancellable *cancellable G_GNUC_UNUSED,
                                   gpointer      user_data)
{
  nuvashalauncher_task_scope_cancel (user_data);
}

void
nuvashalauncher_task_scope_follow_cancellable (nuvashalauncherTaskScope *scope, GCancellable *cancellable)
{
  FollowedCancellable followed;

  g_return_if_fail (scope != NULL);

  /* No cancellable is nothing to follow: the operation is simply not
     tied to anything else. */
  if (cancellable == NULL)
    return;

  /* Following the operation's own cancellable would call
     nuvashalauncher_task_scope_cancel() recursively when that cancellable is cancelled.
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


void
nuvashalauncher_task_scope_take_task (nuvashalauncherTaskScope *scope)
{
  g_return_if_fail (scope != NULL);
  scope->owns_task = TRUE;
}
