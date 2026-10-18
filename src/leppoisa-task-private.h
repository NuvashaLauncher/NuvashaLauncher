/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_TASK_PRIVATE_H
#define leppoisa_TASK_PRIVATE_H

#include "leppoisa-task.h"

/* Internal bridge for operations implemented on top of an existing GTask.
 * Public callers own LeppoisaTaskScope instead of borrowing task ownership. */
LeppoisaTaskScope *leppoisa_task_scope_from_task (GTask *task);
void leppoisa_task_scope_attach_task (GTask *task);
GTask *leppoisa_task_scope_task (const LeppoisaTaskScope *scope);

/* Internal cancellable watch with caller-owned callback data. */
guint leppoisa_cancellable_watch (GCancellable  *cancellable,
                             GSourceFunc    callback,
                             gpointer       user_data,
                             GDestroyNotify notify);

#endif /* leppoisa_TASK_PRIVATE_H */
