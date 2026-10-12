/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-operation.h"

AmlOperation *
aml_operation_new (GObject *source_object, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  return aml_task_scope_new (source_object, cancellable, callback, user_data);
}

void
aml_operation_unref (AmlOperation *operation)
{
  aml_task_scope_unref (operation);
}

void
aml_operation_release (AmlOperation *operation, GCancellable *cancellable)
{
  aml_task_scope_release_when_done (operation, cancellable);
}

AmlOperation *
aml_operation_unref_when_done (AmlOperation *operation)
{
  return aml_task_scope_unref_when_done (operation);
}

void
aml_operation_follow (AmlOperation *operation, GCancellable *cancellable)
{
  aml_task_scope_follow_cancellable (operation, cancellable);
}

void
aml_operation_cancel (AmlOperation *operation)
{
  aml_task_scope_cancel (operation);
}

void
aml_operation_track (AmlOperation *operation, guint source_id)
{
  aml_task_scope_track_source (operation, source_id);
}

void
aml_operation_remove (AmlOperation *operation, guint source_id)
{
  aml_task_scope_remove_source (operation, source_id);
}

void
aml_operation_forget_source (AmlOperation *operation, guint source_id)
{
  aml_task_scope_forget_source (operation, source_id);
}
