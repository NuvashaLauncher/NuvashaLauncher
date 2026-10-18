/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_TASK_H
#define nuvasha_TASK_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NuvashaTaskSequence NuvashaTaskSequence;
typedef struct _NuvashaTaskScope NuvashaTaskScope;
typedef enum {
  nuvasha_TASK_STATE_QUEUED,
  nuvasha_TASK_STATE_RUNNING,
  nuvasha_TASK_STATE_RETRYING,
  nuvasha_TASK_STATE_COMPLETED,
  nuvasha_TASK_STATE_FAILED,
  nuvasha_TASK_STATE_CANCELLED,
  nuvasha_TASK_STATE_CLEANING
} NuvashaTaskState;
typedef void (*NuvashaTaskSequenceStep) (NuvashaTaskSequence *sequence,
                                     gpointer         user_data);

/* A small sequential async-task runner. A step starts one asynchronous
 * operation and calls nuvasha_task_sequence_next() from its completion callback.
 * The sequence owns @user_data until it completes or fails.
 *
 * Completing a sequence frees its step state. Keep a reference with
 * nuvasha_task_sequence_ref() when an asynchronous callback may use the
 * sequence after the completion callback has run. The caller owns the
 * reference returned by this constructor and must release it with
 * nuvasha_task_sequence_unref(), normally after starting the sequence. */
NuvashaTaskSequence *nuvasha_task_sequence_new (GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data,
                                        NuvashaTaskSequenceStep step,
                                        gpointer user_data,
                                        GDestroyNotify user_data_free);
NuvashaTaskSequence *nuvasha_task_sequence_ref (NuvashaTaskSequence *sequence);
void nuvasha_task_sequence_unref (NuvashaTaskSequence *sequence);
void nuvasha_task_sequence_start (NuvashaTaskSequence *sequence);
void nuvasha_task_sequence_next (NuvashaTaskSequence *sequence);
GCancellable *nuvasha_task_sequence_get_cancellable (const NuvashaTaskSequence *sequence);
/* An operation handle for the sequence, for a caller that wants to stop it
 * or to hold one thing for the whole run. It holds its own reference on the
 * task, so releasing it does not stop the sequence. */
NuvashaTaskScope *nuvasha_task_sequence_scope (NuvashaTaskSequence *sequence);
void nuvasha_task_sequence_return_pointer (NuvashaTaskSequence *sequence,
                                       gpointer result,
                                       GDestroyNotify result_free);
void nuvasha_task_sequence_return_error (NuvashaTaskSequence *sequence,
                                     GError *error);

/* Owned operation state for asynchronous work. The scope owns the task's
 * source IDs and removes them before its task data is destroyed. Each owner
 * must hold a reference until it no longer needs the operation; this makes
 * it safe to hand one operation to both the task data and a caller. */
NuvashaTaskScope *nuvasha_task_scope_new (GObject *source_object,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
NuvashaTaskScope *nuvasha_task_scope_ref (NuvashaTaskScope *scope);
void nuvasha_task_scope_unref (NuvashaTaskScope *scope);
guint64 nuvasha_task_scope_get_id (const NuvashaTaskScope *scope);
NuvashaTaskState nuvasha_task_scope_get_state (const NuvashaTaskScope *scope);
void nuvasha_task_scope_set_state (NuvashaTaskScope *scope, NuvashaTaskState state);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaTaskScope, nuvasha_task_scope_unref)
GCancellable *nuvasha_task_scope_get_cancellable (const NuvashaTaskScope *scope);
void nuvasha_task_scope_set_check_cancellable (NuvashaTaskScope *scope,
                                           gboolean       check);
gboolean nuvasha_task_scope_return_error_if_cancelled (NuvashaTaskScope *scope);
void nuvasha_task_scope_return_pointer (NuvashaTaskScope *scope,
                                    gpointer       result,
                                    GDestroyNotify result_free);
void nuvasha_task_scope_return_boolean (NuvashaTaskScope *scope, gboolean result);
void nuvasha_task_scope_return_error (NuvashaTaskScope *scope, GError *error);

/* Completes an operation that has nothing to wait for. The callback runs
 * from an idle, never from inside the *_async() call, so a caller can
 * always store the handle it has to release in the callback. A constructor
 * that can answer on the spot uses these instead of the plain returns. */
void nuvasha_task_scope_return_pointer_deferred (NuvashaTaskScope *scope,
                                              gpointer       result,
                                              GDestroyNotify result_free);
void nuvasha_task_scope_return_error_deferred (NuvashaTaskScope *scope, GError *error);
void nuvasha_task_scope_return_boolean_deferred (NuvashaTaskScope *scope,
                                            gboolean result);
void nuvasha_task_scope_track_source (NuvashaTaskScope *scope, guint source_id);
void nuvasha_task_scope_remove_source (NuvashaTaskScope *scope, guint source_id);
void nuvasha_task_scope_forget_source (NuvashaTaskScope *scope, guint source_id);
void nuvasha_task_scope_cancel (NuvashaTaskScope *scope);

/* Hands the caller's reference back: the operation releases itself once
 * it completes. For work whose answer nobody is waiting for, where the
 * handle exists only to be able to cancel the work. Returns @scope so it
 * can be chained onto a call that hands one back.
 *
 * The release happens on the next main-loop turn, never inside the
 * completion itself: state that borrows the scope is usually freed right
 * after the operation completes, and it must still find the scope there. */
NuvashaTaskScope *nuvasha_task_scope_unref_when_done (NuvashaTaskScope *scope);

/* Hands the caller's reference back and ties the operation to
 * @cancellable. For work whose answer nobody is waiting for but which
 * still has to stop when whatever asked for it is stopped. */
void nuvasha_task_scope_release_when_done (NuvashaTaskScope *scope,
                                       GCancellable *cancellable);

/* Cancels @scope whenever @cancellable is cancelled, for an operation
 * whose cancel button belongs to an object the caller does not own. A
 * NULL @cancellable ties the operation to nothing. The scope stops
 * watching when its last reference goes away. */
void nuvasha_task_scope_follow_cancellable (NuvashaTaskScope *scope,
                                        GCancellable *cancellable);

G_END_DECLS

#endif /* nuvasha_TASK_H */
