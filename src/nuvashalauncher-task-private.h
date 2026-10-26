/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_TASK_PRIVATE_H
#define nuvashalauncher_TASK_PRIVATE_H

#include "nuvashalauncher-task.h"

/* Internal bridge for operations implemented on top of an existing GTask.
 * Public callers own nuvashalauncherTaskScope; the task keeps its own reference. */
nuvashalauncherTaskScope *nuvashalauncher_task_scope_from_task (GTask *task);
void nuvashalauncher_task_scope_attach_task (GTask *task);
GTask *nuvashalauncher_task_scope_task (const nuvashalauncherTaskScope *scope);

/* Internal cancellable watch with caller-owned callback data. */
guint nuvashalauncher_cancellable_watch (GCancellable  *cancellable,
                             GSourceFunc    callback,
                             gpointer       user_data,
                             GDestroyNotify notify);

#endif /* nuvashalauncher_TASK_PRIVATE_H */
