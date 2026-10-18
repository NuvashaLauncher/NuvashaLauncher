/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-operation.h"

ComfyOperation *
comfy_operation_new (GObject *source_object, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  return comfy_task_scope_new (source_object, cancellable, callback, user_data);
}

void
comfy_operation_unref (ComfyOperation *operation)
{
  comfy_task_scope_unref (operation);
}

void
comfy_operation_release (ComfyOperation *operation, GCancellable *cancellable)
{
  comfy_task_scope_release_when_done (operation, cancellable);
}

ComfyOperation *
comfy_operation_unref_when_done (ComfyOperation *operation)
{
  return comfy_task_scope_unref_when_done (operation);
}

void
comfy_operation_follow (ComfyOperation *operation, GCancellable *cancellable)
{
  comfy_task_scope_follow_cancellable (operation, cancellable);
}

void
comfy_operation_cancel (ComfyOperation *operation)
{
  comfy_task_scope_cancel (operation);
}

void
comfy_operation_track (ComfyOperation *operation, guint source_id)
{
  comfy_task_scope_track_source (operation, source_id);
}

void
comfy_operation_remove (ComfyOperation *operation, guint source_id)
{
  comfy_task_scope_remove_source (operation, source_id);
}

void
comfy_operation_forget_source (ComfyOperation *operation, guint source_id)
{
  comfy_task_scope_forget_source (operation, source_id);
}
