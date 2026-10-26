/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_TASK_H
#define nuvashalauncher_TASK_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _nuvashalauncherTaskSequence nuvashalauncherTaskSequence;
typedef struct _nuvashalauncherTaskScope nuvashalauncherTaskScope;
typedef enum {
  nuvashalauncher_TASK_STATE_QUEUED,
  nuvashalauncher_TASK_STATE_RUNNING,
  nuvashalauncher_TASK_STATE_RETRYING,
  nuvashalauncher_TASK_STATE_COMPLETED,
  nuvashalauncher_TASK_STATE_FAILED,
  nuvashalauncher_TASK_STATE_CANCELLED,
  nuvashalauncher_TASK_STATE_CLEANING
} nuvashalauncherTaskState;
typedef void (*nuvashalauncherTaskSequenceStep) (nuvashalauncherTaskSequence *sequence,
                                     gpointer         user_data);

/* A small sequential async-task runner. A step starts one asynchronous
 * operation and calls nuvashalauncher_task_sequence_next() from its completion callback.
 * The sequence owns @user_data until it completes or fails.
 *
 * Completing a sequence frees its step state. Keep a reference with
 * nuvashalauncher_task_sequence_ref() when an asynchronous callback may use the
 * sequence after the completion callback has run. The caller owns the
 * reference returned by this constructor and must release it with
 * nuvashalauncher_task_sequence_unref(), normally after starting the sequence. */
nuvashalauncherTaskSequence *nuvashalauncher_task_sequence_new (GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data,
                                        nuvashalauncherTaskSequenceStep step,
                                        gpointer user_data,
                                        GDestroyNotify user_data_free);
nuvashalauncherTaskSequence *nuvashalauncher_task_sequence_ref (nuvashalauncherTaskSequence *sequence);
void nuvashalauncher_task_sequence_unref (nuvashalauncherTaskSequence *sequence);
void nuvashalauncher_task_sequence_start (nuvashalauncherTaskSequence *sequence);
void nuvashalauncher_task_sequence_next (nuvashalauncherTaskSequence *sequence);
GCancellable *nuvashalauncher_task_sequence_get_cancellable (const nuvashalauncherTaskSequence *sequence);
/* An operation handle for the sequence, for a caller that wants to stop it
 * or to hold one thing for the whole run. It holds its own reference on the
 * task, so releasing it does not stop the sequence. */
nuvashalauncherTaskScope *nuvashalauncher_task_sequence_scope (nuvashalauncherTaskSequence *sequence);
void nuvashalauncher_task_sequence_return_pointer (nuvashalauncherTaskSequence *sequence,
                                       gpointer result,
                                       GDestroyNotify result_free);
void nuvashalauncher_task_sequence_return_error (nuvashalauncherTaskSequence *sequence,
                                     GError *error);

/* Owned operation state for asynchronous work. The scope owns the task's
 * source IDs and removes them before its task data is destroyed. Each owner
 * must hold a reference until it no longer needs the operation; this makes
 * it safe to hand one operation to both the task data and a caller. */
nuvashalauncherTaskScope *nuvashalauncher_task_scope_new_tagged (GObject *source_object,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data,
                                          const char *source_tag);
#define nuvashalauncher_task_scope_new(source_object, cancellable, callback, user_data) \
  nuvashalauncher_task_scope_new_tagged ((source_object), (cancellable), (callback), \
                                 (user_data), G_STRFUNC)
nuvashalauncherTaskScope *nuvashalauncher_task_scope_ref (nuvashalauncherTaskScope *scope);
void nuvashalauncher_task_scope_unref (nuvashalauncherTaskScope *scope);
guint64 nuvashalauncher_task_scope_get_id (const nuvashalauncherTaskScope *scope);
nuvashalauncherTaskState nuvashalauncher_task_scope_get_state (const nuvashalauncherTaskScope *scope);
void nuvashalauncher_task_scope_set_source_tag (nuvashalauncherTaskScope *scope,
                                        gpointer          source_tag);
gboolean nuvashalauncher_task_result_is_valid (GAsyncResult *result,
                                       const char *expected_tag);
void nuvashalauncher_task_scope_set_state (nuvashalauncherTaskScope *scope, nuvashalauncherTaskState state);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherTaskScope, nuvashalauncher_task_scope_unref)
GCancellable *nuvashalauncher_task_scope_get_cancellable (const nuvashalauncherTaskScope *scope);
void nuvashalauncher_task_scope_set_check_cancellable (nuvashalauncherTaskScope *scope,
                                           gboolean       check);
gboolean nuvashalauncher_task_scope_return_error_if_cancelled (nuvashalauncherTaskScope *scope);
void nuvashalauncher_task_scope_return_pointer (nuvashalauncherTaskScope *scope,
                                    gpointer       result,
                                    GDestroyNotify result_free);
void nuvashalauncher_task_scope_return_boolean (nuvashalauncherTaskScope *scope, gboolean result);
void nuvashalauncher_task_scope_return_error (nuvashalauncherTaskScope *scope, GError *error);

/* Completes an operation that has nothing to wait for. The callback runs
 * from an idle, never from inside the *_async() call, so a caller can
 * always store the handle it has to release in the callback. A constructor
 * that can answer on the spot uses these instead of the plain returns. */
void nuvashalauncher_task_scope_return_pointer_deferred (nuvashalauncherTaskScope *scope,
                                              gpointer       result,
                                              GDestroyNotify result_free);
void nuvashalauncher_task_scope_return_error_deferred (nuvashalauncherTaskScope *scope, GError *error);
void nuvashalauncher_task_scope_return_boolean_deferred (nuvashalauncherTaskScope *scope,
                                            gboolean result);
void nuvashalauncher_task_scope_track_source (nuvashalauncherTaskScope *scope, guint source_id);
void nuvashalauncher_task_scope_remove_source (nuvashalauncherTaskScope *scope, guint source_id);
void nuvashalauncher_task_scope_forget_source (nuvashalauncherTaskScope *scope, guint source_id);
void nuvashalauncher_task_scope_cancel (nuvashalauncherTaskScope *scope);

/* Hands the caller's reference back: the operation releases itself once
 * it completes. For work whose answer nobody is waiting for, where the
 * handle exists only to be able to cancel the work. Returns @scope so it
 * can be chained onto a call that hands one back.
 *
 * The release happens on the next main-loop turn, never inside the
 * completion itself: state that borrows the scope is usually freed right
 * after the operation completes, and it must still find the scope there. */
nuvashalauncherTaskScope *nuvashalauncher_task_scope_unref_when_done (nuvashalauncherTaskScope *scope);

/* Hands the caller's reference back and ties the operation to
 * @cancellable. For work whose answer nobody is waiting for but which
 * still has to stop when whatever asked for it is stopped. */
void nuvashalauncher_task_scope_release_when_done (nuvashalauncherTaskScope *scope,
                                       GCancellable *cancellable);

/* Cancels @scope whenever @cancellable is cancelled, for an operation
 * whose cancel button belongs to an object the caller does not own. A
 * NULL @cancellable ties the operation to nothing. The scope stops
 * watching when its last reference goes away. */
void nuvashalauncher_task_scope_follow_cancellable (nuvashalauncherTaskScope *scope,
                                        GCancellable *cancellable);

G_END_DECLS

#endif /* nuvashalauncher_TASK_H */
