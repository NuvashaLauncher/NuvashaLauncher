/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_OPERATION_H
#define nuvashalauncher_OPERATION_H

#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

typedef NuvashaLauncherTaskScope NuvashaLauncherOperation;

NuvashaLauncherOperation *nuvashalauncher_operation_new (GObject *source_object,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
void nuvashalauncher_operation_unref (NuvashaLauncherOperation *operation);
void nuvashalauncher_operation_release (NuvashaLauncherOperation *operation,
                            GCancellable *cancellable);
NuvashaLauncherOperation *nuvashalauncher_operation_unref_when_done (NuvashaLauncherOperation *operation);
void nuvashalauncher_operation_follow (NuvashaLauncherOperation *operation,
                           GCancellable *cancellable);
void nuvashalauncher_operation_cancel (NuvashaLauncherOperation *operation);
void nuvashalauncher_operation_track (NuvashaLauncherOperation *operation, guint source_id);
void nuvashalauncher_operation_remove (NuvashaLauncherOperation *operation, guint source_id);
void nuvashalauncher_operation_forget_source (NuvashaLauncherOperation *operation, guint source_id);

G_END_DECLS

#endif /* nuvashalauncher_OPERATION_H */
