/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_TASK_H
#define comfy_TASK_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _ComfyTaskSequence ComfyTaskSequence;
typedef struct _ComfyTaskScope ComfyTaskScope;
typedef enum {
  comfy_TASK_STATE_QUEUED,
  comfy_TASK_STATE_RUNNING,
  comfy_TASK_STATE_RETRYING,
  comfy_TASK_STATE_COMPLETED,
  comfy_TASK_STATE_FAILED,
  comfy_TASK_STATE_CANCELLED,
  comfy_TASK_STATE_CLEANING
} ComfyTaskState;
typedef void (*ComfyTaskSequenceStep) (ComfyTaskSequence *sequence,
                                     gpointer         user_data);

/* A small sequential async-task runner. A step starts one asynchronous
 * operation and calls comfy_task_sequence_next() from its completion callback.
 * The sequence owns @user_data until it completes or fails.
 *
 * Completing a sequence frees its step state. Keep a reference with
 * comfy_task_sequence_ref() when an asynchronous callback may use the
 * sequence after the completion callback has run. The caller owns the
 * reference returned by this constructor and must release it with
 * comfy_task_sequence_unref(), normally after starting the sequence. */
ComfyTaskSequence *comfy_task_sequence_new (GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data,
                                        ComfyTaskSequenceStep step,
                                        gpointer user_data,
                                        GDestroyNotify user_data_free);
ComfyTaskSequence *comfy_task_sequence_ref (ComfyTaskSequence *sequence);
void comfy_task_sequence_unref (ComfyTaskSequence *sequence);
void comfy_task_sequence_start (ComfyTaskSequence *sequence);
void comfy_task_sequence_next (ComfyTaskSequence *sequence);
GCancellable *comfy_task_sequence_get_cancellable (const ComfyTaskSequence *sequence);
/* An operation handle for the sequence, for a caller that wants to stop it
 * or to hold one thing for the whole run. It holds its own reference on the
 * task, so releasing it does not stop the sequence. */
ComfyTaskScope *comfy_task_sequence_scope (ComfyTaskSequence *sequence);
void comfy_task_sequence_return_pointer (ComfyTaskSequence *sequence,
                                       gpointer result,
                                       GDestroyNotify result_free);
void comfy_task_sequence_return_error (ComfyTaskSequence *sequence,
                                     GError *error);

/* Owned operation state for asynchronous work. The scope owns the task's
 * source IDs and removes them before its task data is destroyed. Each owner
 * must hold a reference until it no longer needs the operation; this makes
 * it safe to hand one operation to both the task data and a caller. */
ComfyTaskScope *comfy_task_scope_new (GObject *source_object,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
ComfyTaskScope *comfy_task_scope_ref (ComfyTaskScope *scope);
void comfy_task_scope_unref (ComfyTaskScope *scope);
guint64 comfy_task_scope_get_id (const ComfyTaskScope *scope);
ComfyTaskState comfy_task_scope_get_state (const ComfyTaskScope *scope);
void comfy_task_scope_set_state (ComfyTaskScope *scope, ComfyTaskState state);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyTaskScope, comfy_task_scope_unref)
GCancellable *comfy_task_scope_get_cancellable (const ComfyTaskScope *scope);
void comfy_task_scope_set_check_cancellable (ComfyTaskScope *scope,
                                           gboolean       check);
gboolean comfy_task_scope_return_error_if_cancelled (ComfyTaskScope *scope);
void comfy_task_scope_return_pointer (ComfyTaskScope *scope,
                                    gpointer       result,
                                    GDestroyNotify result_free);
void comfy_task_scope_return_boolean (ComfyTaskScope *scope, gboolean result);
void comfy_task_scope_return_error (ComfyTaskScope *scope, GError *error);

/* Completes an operation that has nothing to wait for. The callback runs
 * from an idle, never from inside the *_async() call, so a caller can
 * always store the handle it has to release in the callback. A constructor
 * that can answer on the spot uses these instead of the plain returns. */
void comfy_task_scope_return_pointer_deferred (ComfyTaskScope *scope,
                                              gpointer       result,
                                              GDestroyNotify result_free);
void comfy_task_scope_return_error_deferred (ComfyTaskScope *scope, GError *error);
void comfy_task_scope_return_boolean_deferred (ComfyTaskScope *scope,
                                            gboolean result);
void comfy_task_scope_track_source (ComfyTaskScope *scope, guint source_id);
void comfy_task_scope_remove_source (ComfyTaskScope *scope, guint source_id);
void comfy_task_scope_forget_source (ComfyTaskScope *scope, guint source_id);
void comfy_task_scope_cancel (ComfyTaskScope *scope);

/* Hands the caller's reference back: the operation releases itself once
 * it completes. For work whose answer nobody is waiting for, where the
 * handle exists only to be able to cancel the work. Returns @scope so it
 * can be chained onto a call that hands one back.
 *
 * The release happens on the next main-loop turn, never inside the
 * completion itself: state that borrows the scope is usually freed right
 * after the operation completes, and it must still find the scope there. */
ComfyTaskScope *comfy_task_scope_unref_when_done (ComfyTaskScope *scope);

/* Hands the caller's reference back and ties the operation to
 * @cancellable. For work whose answer nobody is waiting for but which
 * still has to stop when whatever asked for it is stopped. */
void comfy_task_scope_release_when_done (ComfyTaskScope *scope,
                                       GCancellable *cancellable);

/* Cancels @scope whenever @cancellable is cancelled, for an operation
 * whose cancel button belongs to an object the caller does not own. A
 * NULL @cancellable ties the operation to nothing. The scope stops
 * watching when its last reference goes away. */
void comfy_task_scope_follow_cancellable (ComfyTaskScope *scope,
                                        GCancellable *cancellable);

G_END_DECLS

#endif /* comfy_TASK_H */
