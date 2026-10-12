/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_TASK_PRIVATE_H
#define AML_TASK_PRIVATE_H

#include "aml-task.h"

/* Internal bridge for operations implemented on top of an existing GTask.
 * Public callers own AmlTaskScope instead of borrowing task ownership. */
AmlTaskScope *aml_task_scope_from_task (GTask *task);
void aml_task_scope_attach_task (GTask *task);
GTask *aml_task_scope_task (const AmlTaskScope *scope);

#endif /* AML_TASK_PRIVATE_H */
