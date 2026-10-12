/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_OPERATION_H
#define AML_OPERATION_H

#include "aml-task.h"

G_BEGIN_DECLS

typedef AmlTaskScope AmlOperation;

AmlOperation *aml_operation_new (GObject *source_object,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
void aml_operation_unref (AmlOperation *operation);
void aml_operation_release (AmlOperation *operation,
                            GCancellable *cancellable);
AmlOperation *aml_operation_unref_when_done (AmlOperation *operation);
void aml_operation_follow (AmlOperation *operation,
                           GCancellable *cancellable);
void aml_operation_cancel (AmlOperation *operation);
void aml_operation_track (AmlOperation *operation, guint source_id);
void aml_operation_remove (AmlOperation *operation, guint source_id);
void aml_operation_forget_source (AmlOperation *operation, guint source_id);

G_END_DECLS

#endif /* AML_OPERATION_H */
