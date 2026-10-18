/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-operation.h"

NuvashaOperation *
nuvasha_operation_new (GObject *source_object, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  return nuvasha_task_scope_new (source_object, cancellable, callback, user_data);
}

void
nuvasha_operation_unref (NuvashaOperation *operation)
{
  nuvasha_task_scope_unref (operation);
}

void
nuvasha_operation_release (NuvashaOperation *operation, GCancellable *cancellable)
{
  nuvasha_task_scope_release_when_done (operation, cancellable);
}

NuvashaOperation *
nuvasha_operation_unref_when_done (NuvashaOperation *operation)
{
  return nuvasha_task_scope_unref_when_done (operation);
}

void
nuvasha_operation_follow (NuvashaOperation *operation, GCancellable *cancellable)
{
  nuvasha_task_scope_follow_cancellable (operation, cancellable);
}

void
nuvasha_operation_cancel (NuvashaOperation *operation)
{
  nuvasha_task_scope_cancel (operation);
}

void
nuvasha_operation_track (NuvashaOperation *operation, guint source_id)
{
  nuvasha_task_scope_track_source (operation, source_id);
}

void
nuvasha_operation_remove (NuvashaOperation *operation, guint source_id)
{
  nuvasha_task_scope_remove_source (operation, source_id);
}

void
nuvasha_operation_forget_source (NuvashaOperation *operation, guint source_id)
{
  nuvasha_task_scope_forget_source (operation, source_id);
}
