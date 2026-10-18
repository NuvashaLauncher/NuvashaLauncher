/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_TASK_H
#define leppoisa_TASK_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _LeppoisaTaskSequence LeppoisaTaskSequence;
typedef struct _LeppoisaTaskScope LeppoisaTaskScope;
typedef enum {
  leppoisa_TASK_STATE_QUEUED,
  leppoisa_TASK_STATE_RUNNING,
  leppoisa_TASK_STATE_RETRYING,
  leppoisa_TASK_STATE_COMPLETED,
  leppoisa_TASK_STATE_FAILED,
  leppoisa_TASK_STATE_CANCELLED,
  leppoisa_TASK_STATE_CLEANING
} LeppoisaTaskState;
typedef void (*LeppoisaTaskSequenceStep) (LeppoisaTaskSequence *sequence,
                                     gpointer         user_data);

/* A small sequential async-task runner. A step starts one asynchronous
 * operation and calls leppoisa_task_sequence_next() from its completion callback.
 * The sequence owns @user_data until it completes or fails.
 *
 * Completing a sequence frees its step state. Keep a reference with
 * leppoisa_task_sequence_ref() when an asynchronous callback may use the
 * sequence after the completion callback has run. The caller owns the
 * reference returned by this constructor and must release it with
 * leppoisa_task_sequence_unref(), normally after starting the sequence. */
LeppoisaTaskSequence *leppoisa_task_sequence_new (GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data,
                                        LeppoisaTaskSequenceStep step,
                                        gpointer user_data,
                                        GDestroyNotify user_data_free);
LeppoisaTaskSequence *leppoisa_task_sequence_ref (LeppoisaTaskSequence *sequence);
void leppoisa_task_sequence_unref (LeppoisaTaskSequence *sequence);
void leppoisa_task_sequence_start (LeppoisaTaskSequence *sequence);
void leppoisa_task_sequence_next (LeppoisaTaskSequence *sequence);
GCancellable *leppoisa_task_sequence_get_cancellable (const LeppoisaTaskSequence *sequence);
/* An operation handle for the sequence, for a caller that wants to stop it
 * or to hold one thing for the whole run. It holds its own reference on the
 * task, so releasing it does not stop the sequence. */
LeppoisaTaskScope *leppoisa_task_sequence_scope (LeppoisaTaskSequence *sequence);
void leppoisa_task_sequence_return_pointer (LeppoisaTaskSequence *sequence,
                                       gpointer result,
                                       GDestroyNotify result_free);
void leppoisa_task_sequence_return_error (LeppoisaTaskSequence *sequence,
                                     GError *error);

/* Owned operation state for asynchronous work. The scope owns the task's
 * source IDs and removes them before its task data is destroyed. Each owner
 * must hold a reference until it no longer needs the operation; this makes
 * it safe to hand one operation to both the task data and a caller. */
LeppoisaTaskScope *leppoisa_task_scope_new (GObject *source_object,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
LeppoisaTaskScope *leppoisa_task_scope_ref (LeppoisaTaskScope *scope);
void leppoisa_task_scope_unref (LeppoisaTaskScope *scope);
guint64 leppoisa_task_scope_get_id (const LeppoisaTaskScope *scope);
LeppoisaTaskState leppoisa_task_scope_get_state (const LeppoisaTaskScope *scope);
void leppoisa_task_scope_set_state (LeppoisaTaskScope *scope, LeppoisaTaskState state);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (LeppoisaTaskScope, leppoisa_task_scope_unref)
GCancellable *leppoisa_task_scope_get_cancellable (const LeppoisaTaskScope *scope);
void leppoisa_task_scope_set_check_cancellable (LeppoisaTaskScope *scope,
                                           gboolean       check);
gboolean leppoisa_task_scope_return_error_if_cancelled (LeppoisaTaskScope *scope);
void leppoisa_task_scope_return_pointer (LeppoisaTaskScope *scope,
                                    gpointer       result,
                                    GDestroyNotify result_free);
void leppoisa_task_scope_return_boolean (LeppoisaTaskScope *scope, gboolean result);
void leppoisa_task_scope_return_error (LeppoisaTaskScope *scope, GError *error);

/* Completes an operation that has nothing to wait for. The callback runs
 * from an idle, never from inside the *_async() call, so a caller can
 * always store the handle it has to release in the callback. A constructor
 * that can answer on the spot uses these instead of the plain returns. */
void leppoisa_task_scope_return_pointer_deferred (LeppoisaTaskScope *scope,
                                              gpointer       result,
                                              GDestroyNotify result_free);
void leppoisa_task_scope_return_error_deferred (LeppoisaTaskScope *scope, GError *error);
void leppoisa_task_scope_return_boolean_deferred (LeppoisaTaskScope *scope,
                                            gboolean result);
void leppoisa_task_scope_track_source (LeppoisaTaskScope *scope, guint source_id);
void leppoisa_task_scope_remove_source (LeppoisaTaskScope *scope, guint source_id);
void leppoisa_task_scope_forget_source (LeppoisaTaskScope *scope, guint source_id);
void leppoisa_task_scope_cancel (LeppoisaTaskScope *scope);

/* Hands the caller's reference back: the operation releases itself once
 * it completes. For work whose answer nobody is waiting for, where the
 * handle exists only to be able to cancel the work. Returns @scope so it
 * can be chained onto a call that hands one back.
 *
 * The release happens on the next main-loop turn, never inside the
 * completion itself: state that borrows the scope is usually freed right
 * after the operation completes, and it must still find the scope there. */
LeppoisaTaskScope *leppoisa_task_scope_unref_when_done (LeppoisaTaskScope *scope);

/* Hands the caller's reference back and ties the operation to
 * @cancellable. For work whose answer nobody is waiting for but which
 * still has to stop when whatever asked for it is stopped. */
void leppoisa_task_scope_release_when_done (LeppoisaTaskScope *scope,
                                       GCancellable *cancellable);

/* Cancels @scope whenever @cancellable is cancelled, for an operation
 * whose cancel button belongs to an object the caller does not own. A
 * NULL @cancellable ties the operation to nothing. The scope stops
 * watching when its last reference goes away. */
void leppoisa_task_scope_follow_cancellable (LeppoisaTaskScope *scope,
                                        GCancellable *cancellable);

G_END_DECLS

#endif /* leppoisa_TASK_H */
