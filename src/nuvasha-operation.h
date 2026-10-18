/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_OPERATION_H
#define nuvasha_OPERATION_H

#include "nuvasha-task.h"

G_BEGIN_DECLS

typedef NuvashaTaskScope NuvashaOperation;

NuvashaOperation *nuvasha_operation_new (GObject *source_object,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
void nuvasha_operation_unref (NuvashaOperation *operation);
void nuvasha_operation_release (NuvashaOperation *operation,
                            GCancellable *cancellable);
NuvashaOperation *nuvasha_operation_unref_when_done (NuvashaOperation *operation);
void nuvasha_operation_follow (NuvashaOperation *operation,
                           GCancellable *cancellable);
void nuvasha_operation_cancel (NuvashaOperation *operation);
void nuvasha_operation_track (NuvashaOperation *operation, guint source_id);
void nuvasha_operation_remove (NuvashaOperation *operation, guint source_id);
void nuvasha_operation_forget_source (NuvashaOperation *operation, guint source_id);

G_END_DECLS

#endif /* nuvasha_OPERATION_H */
