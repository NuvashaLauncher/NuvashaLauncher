/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_TASK_H
#define AML_TASK_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _AmlTaskSequence AmlTaskSequence;
typedef struct _AmlTaskScope AmlTaskScope;
typedef enum {
  AML_TASK_STATE_QUEUED,
  AML_TASK_STATE_RUNNING,
  AML_TASK_STATE_RETRYING,
  AML_TASK_STATE_COMPLETED,
  AML_TASK_STATE_FAILED,
  AML_TASK_STATE_CANCELLED,
  AML_TASK_STATE_CLEANING
} AmlTaskState;
typedef void (*AmlTaskSequenceStep) (AmlTaskSequence *sequence,
                                     gpointer         user_data);

/* A small sequential async-task runner. A step starts one asynchronous
 * operation and calls aml_task_sequence_next() from its completion callback.
 * The sequence owns @user_data until it completes or fails.
 *
 * Completing a sequence frees its step state. Keep a reference with
 * aml_task_sequence_ref() when an asynchronous callback may use the
 * sequence after the completion callback has run. The caller owns the
 * reference returned by this constructor and must release it with
 * aml_task_sequence_unref(), normally after starting the sequence. */
AmlTaskSequence *aml_task_sequence_new (GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data,
                                        AmlTaskSequenceStep step,
                                        gpointer user_data,
                                        GDestroyNotify user_data_free);
AmlTaskSequence *aml_task_sequence_ref (AmlTaskSequence *sequence);
void aml_task_sequence_unref (AmlTaskSequence *sequence);
void aml_task_sequence_start (AmlTaskSequence *sequence);
void aml_task_sequence_next (AmlTaskSequence *sequence);
GCancellable *aml_task_sequence_get_cancellable (const AmlTaskSequence *sequence);
/* An operation handle for the sequence, for a caller that wants to stop it
 * or to hold one thing for the whole run. It holds its own reference on the
 * task, so releasing it does not stop the sequence. */
AmlTaskScope *aml_task_sequence_scope (AmlTaskSequence *sequence);
void aml_task_sequence_return_pointer (AmlTaskSequence *sequence,
                                       gpointer result,
                                       GDestroyNotify result_free);
void aml_task_sequence_return_error (AmlTaskSequence *sequence,
                                     GError *error);

/* Owned operation state for asynchronous work. The scope owns the task's
 * source IDs and removes them before its task data is destroyed. Each owner
 * must hold a reference until it no longer needs the operation; this makes
 * it safe to hand one operation to both the task data and a caller. */
AmlTaskScope *aml_task_scope_new (GObject *source_object,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
AmlTaskScope *aml_task_scope_ref (AmlTaskScope *scope);
void aml_task_scope_unref (AmlTaskScope *scope);
guint64 aml_task_scope_get_id (const AmlTaskScope *scope);
AmlTaskState aml_task_scope_get_state (const AmlTaskScope *scope);
void aml_task_scope_set_state (AmlTaskScope *scope, AmlTaskState state);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlTaskScope, aml_task_scope_unref)
GCancellable *aml_task_scope_get_cancellable (const AmlTaskScope *scope);
void aml_task_scope_set_check_cancellable (AmlTaskScope *scope,
                                           gboolean       check);
gboolean aml_task_scope_return_error_if_cancelled (AmlTaskScope *scope);
void aml_task_scope_return_pointer (AmlTaskScope *scope,
                                    gpointer       result,
                                    GDestroyNotify result_free);
void aml_task_scope_return_boolean (AmlTaskScope *scope, gboolean result);
void aml_task_scope_return_error (AmlTaskScope *scope, GError *error);

/* Completes an operation that has nothing to wait for. The callback runs
 * from an idle, never from inside the *_async() call, so a caller can
 * always store the handle it has to release in the callback. A constructor
 * that can answer on the spot uses these instead of the plain returns. */
void aml_task_scope_return_pointer_deferred (AmlTaskScope *scope,
                                             gpointer       result,
                                             GDestroyNotify result_free);
void aml_task_scope_return_boolean_deferred (AmlTaskScope *scope,
                                             gboolean       result);
void aml_task_scope_return_error_deferred (AmlTaskScope *scope, GError *error);
void aml_task_scope_track_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_remove_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_forget_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_cancel (AmlTaskScope *scope);

/* Hands the caller's reference back: the operation releases itself once
 * it completes. For work whose answer nobody is waiting for, where the
 * handle exists only to be able to cancel the work. Returns @scope so it
 * can be chained onto a call that hands one back.
 *
 * The release happens on the next main-loop turn, never inside the
 * completion itself: state that borrows the scope is usually freed right
 * after the operation completes, and it must still find the scope there. */
AmlTaskScope *aml_task_scope_unref_when_done (AmlTaskScope *scope);

/* Hands the caller's reference back and ties the operation to
 * @cancellable. For work whose answer nobody is waiting for but which
 * still has to stop when whatever asked for it is stopped. */
void aml_task_scope_release_when_done (AmlTaskScope *scope,
                                       GCancellable *cancellable);

/* Cancels @scope whenever @cancellable is cancelled, for an operation
 * whose cancel button belongs to an object the caller does not own. A
 * NULL @cancellable ties the operation to nothing. The scope stops
 * watching when its last reference goes away. */
void aml_task_scope_follow_cancellable (AmlTaskScope *scope,
                                        GCancellable *cancellable);

G_END_DECLS

#endif /* AML_TASK_H */
