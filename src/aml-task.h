/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_TASK_H
#define AML_TASK_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _AmlTaskSequence AmlTaskSequence;
typedef struct _AmlTaskScope AmlTaskScope;
typedef void (*AmlTaskSequenceStep) (GTask *task, gpointer user_data);

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
GTask *aml_task_sequence_get_task (const AmlTaskSequence *sequence);
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
AmlTaskScope *aml_task_scope_borrow_task (GTask *task);
AmlTaskScope *aml_task_scope_ref (AmlTaskScope *scope);
void aml_task_scope_unref (AmlTaskScope *scope);
/* Attach an operation scope to @task. The task owns the attached scope. */
void aml_task_scope_attach_task (GTask *task);
GTask *aml_task_scope_get_task (const AmlTaskScope *scope);
void aml_task_scope_track_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_remove_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_forget_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_cancel (AmlTaskScope *scope);
void aml_task_scope_free (AmlTaskScope *scope);

G_END_DECLS

#endif /* AML_TASK_H */
