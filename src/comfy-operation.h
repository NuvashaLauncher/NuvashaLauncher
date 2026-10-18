/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_OPERATION_H
#define comfy_OPERATION_H

#include "comfy-task.h"

G_BEGIN_DECLS

typedef ComfyTaskScope ComfyOperation;

ComfyOperation *comfy_operation_new (GObject *source_object,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
void comfy_operation_unref (ComfyOperation *operation);
void comfy_operation_release (ComfyOperation *operation,
                            GCancellable *cancellable);
ComfyOperation *comfy_operation_unref_when_done (ComfyOperation *operation);
void comfy_operation_follow (ComfyOperation *operation,
                           GCancellable *cancellable);
void comfy_operation_cancel (ComfyOperation *operation);
void comfy_operation_track (ComfyOperation *operation, guint source_id);
void comfy_operation_remove (ComfyOperation *operation, guint source_id);
void comfy_operation_forget_source (ComfyOperation *operation, guint source_id);

G_END_DECLS

#endif /* comfy_OPERATION_H */
