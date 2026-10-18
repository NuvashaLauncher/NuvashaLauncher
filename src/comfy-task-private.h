/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_TASK_PRIVATE_H
#define comfy_TASK_PRIVATE_H

#include "comfy-task.h"

/* Internal bridge for operations implemented on top of an existing GTask.
 * Public callers own ComfyTaskScope instead of borrowing task ownership. */
ComfyTaskScope *comfy_task_scope_from_task (GTask *task);
void comfy_task_scope_attach_task (GTask *task);
GTask *comfy_task_scope_task (const ComfyTaskScope *scope);

/* Internal cancellable watch with caller-owned callback data. */
guint comfy_cancellable_watch (GCancellable  *cancellable,
                             GSourceFunc    callback,
                             gpointer       user_data,
                             GDestroyNotify notify);

#endif /* comfy_TASK_PRIVATE_H */
