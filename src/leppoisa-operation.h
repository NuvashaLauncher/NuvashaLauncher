/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_OPERATION_H
#define leppoisa_OPERATION_H

#include "leppoisa-task.h"

G_BEGIN_DECLS

typedef LeppoisaTaskScope LeppoisaOperation;

LeppoisaOperation *leppoisa_operation_new (GObject *source_object,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
void leppoisa_operation_unref (LeppoisaOperation *operation);
void leppoisa_operation_release (LeppoisaOperation *operation,
                            GCancellable *cancellable);
LeppoisaOperation *leppoisa_operation_unref_when_done (LeppoisaOperation *operation);
void leppoisa_operation_follow (LeppoisaOperation *operation,
                           GCancellable *cancellable);
void leppoisa_operation_cancel (LeppoisaOperation *operation);
void leppoisa_operation_track (LeppoisaOperation *operation, guint source_id);
void leppoisa_operation_remove (LeppoisaOperation *operation, guint source_id);
void leppoisa_operation_forget_source (LeppoisaOperation *operation, guint source_id);

G_END_DECLS

#endif /* leppoisa_OPERATION_H */
