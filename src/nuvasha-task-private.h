/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_TASK_PRIVATE_H
#define nuvasha_TASK_PRIVATE_H

#include "nuvasha-task.h"

/* Internal bridge for operations implemented on top of an existing GTask.
 * Public callers own NuvashaTaskScope; the task keeps its own reference. */
NuvashaTaskScope *nuvasha_task_scope_from_task (GTask *task);
void nuvasha_task_scope_attach_task (GTask *task);
GTask *nuvasha_task_scope_task (const NuvashaTaskScope *scope);

/* Internal cancellable watch with caller-owned callback data. */
guint nuvasha_cancellable_watch (GCancellable  *cancellable,
                             GSourceFunc    callback,
                             gpointer       user_data,
                             GDestroyNotify notify);

#endif /* nuvasha_TASK_PRIVATE_H */
