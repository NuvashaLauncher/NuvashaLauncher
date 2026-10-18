/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-operation.h"

LeppoisaOperation *
leppoisa_operation_new (GObject *source_object, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  return leppoisa_task_scope_new (source_object, cancellable, callback, user_data);
}

void
leppoisa_operation_unref (LeppoisaOperation *operation)
{
  leppoisa_task_scope_unref (operation);
}

void
leppoisa_operation_release (LeppoisaOperation *operation, GCancellable *cancellable)
{
  leppoisa_task_scope_release_when_done (operation, cancellable);
}

LeppoisaOperation *
leppoisa_operation_unref_when_done (LeppoisaOperation *operation)
{
  return leppoisa_task_scope_unref_when_done (operation);
}

void
leppoisa_operation_follow (LeppoisaOperation *operation, GCancellable *cancellable)
{
  leppoisa_task_scope_follow_cancellable (operation, cancellable);
}

void
leppoisa_operation_cancel (LeppoisaOperation *operation)
{
  leppoisa_task_scope_cancel (operation);
}

void
leppoisa_operation_track (LeppoisaOperation *operation, guint source_id)
{
  leppoisa_task_scope_track_source (operation, source_id);
}

void
leppoisa_operation_remove (LeppoisaOperation *operation, guint source_id)
{
  leppoisa_task_scope_remove_source (operation, source_id);
}

void
leppoisa_operation_forget_source (LeppoisaOperation *operation, guint source_id)
{
  leppoisa_task_scope_forget_source (operation, source_id);
}
