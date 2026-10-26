/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_OPERATION_H
#define nuvashalauncher_OPERATION_H

#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

typedef nuvashalauncherTaskScope nuvashalauncherOperation;

nuvashalauncherOperation *nuvashalauncher_operation_new (GObject *source_object,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
void nuvashalauncher_operation_unref (nuvashalauncherOperation *operation);
void nuvashalauncher_operation_release (nuvashalauncherOperation *operation,
                            GCancellable *cancellable);
nuvashalauncherOperation *nuvashalauncher_operation_unref_when_done (nuvashalauncherOperation *operation);
void nuvashalauncher_operation_follow (nuvashalauncherOperation *operation,
                           GCancellable *cancellable);
void nuvashalauncher_operation_cancel (nuvashalauncherOperation *operation);
void nuvashalauncher_operation_track (nuvashalauncherOperation *operation, guint source_id);
void nuvashalauncher_operation_remove (nuvashalauncherOperation *operation, guint source_id);
void nuvashalauncher_operation_forget_source (nuvashalauncherOperation *operation, guint source_id);

G_END_DECLS

#endif /* nuvashalauncher_OPERATION_H */
