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
 * Completing a sequence frees its step state but not the sequence itself:
 * it stays alive with the task until that is done with, so a completion
 * that arrives twice reads a real finished flag and stops, rather than
 * reading freed memory. Completing twice is therefore safe. */
AmlTaskSequence *aml_task_sequence_new (GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data,
                                        AmlTaskSequenceStep step,
                                        gpointer user_data,
                                        GDestroyNotify user_data_free);
void aml_task_sequence_start (AmlTaskSequence *sequence);
void aml_task_sequence_next (AmlTaskSequence *sequence);
GTask *aml_task_sequence_get_task (const AmlTaskSequence *sequence);
void aml_task_sequence_return_pointer (AmlTaskSequence *sequence,
                                       gpointer result,
                                       GDestroyNotify result_free);
void aml_task_sequence_return_error (AmlTaskSequence *sequence,
                                     GError *error);

/* Lifetime scope for asynchronous work. The scope owns the task's source
 * IDs and removes them before its task data is destroyed. Callback code may
 * keep the scope only until it returns from the callback. */
AmlTaskScope *aml_task_scope_new (GObject *source_object,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
AmlTaskScope *aml_task_scope_borrow_task (GTask *task);
GTask *aml_task_scope_get_task (const AmlTaskScope *scope);
void aml_task_scope_track_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_remove_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_forget_source (AmlTaskScope *scope, guint source_id);
void aml_task_scope_cancel (AmlTaskScope *scope);
void aml_task_scope_free (AmlTaskScope *scope);

G_END_DECLS

#endif /* AML_TASK_H */
