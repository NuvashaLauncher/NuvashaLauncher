/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-operation.h"

NuvashaLauncherOperation *
nuvashalauncher_operation_new (GObject *source_object, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  return nuvashalauncher_task_scope_new (source_object, cancellable, callback, user_data);
}

void
nuvashalauncher_operation_unref (NuvashaLauncherOperation *operation)
{
  nuvashalauncher_task_scope_unref (operation);
}

void
nuvashalauncher_operation_release (NuvashaLauncherOperation *operation, GCancellable *cancellable)
{
  nuvashalauncher_task_scope_release_when_done (operation, cancellable);
}

NuvashaLauncherOperation *
nuvashalauncher_operation_unref_when_done (NuvashaLauncherOperation *operation)
{
  return nuvashalauncher_task_scope_unref_when_done (operation);
}

void
nuvashalauncher_operation_follow (NuvashaLauncherOperation *operation, GCancellable *cancellable)
{
  nuvashalauncher_task_scope_follow_cancellable (operation, cancellable);
}

void
nuvashalauncher_operation_cancel (NuvashaLauncherOperation *operation)
{
  nuvashalauncher_task_scope_cancel (operation);
}

void
nuvashalauncher_operation_track (NuvashaLauncherOperation *operation, guint source_id)
{
  nuvashalauncher_task_scope_track_source (operation, source_id);
}

void
nuvashalauncher_operation_remove (NuvashaLauncherOperation *operation, guint source_id)
{
  nuvashalauncher_task_scope_remove_source (operation, source_id);
}

void
nuvashalauncher_operation_forget_source (NuvashaLauncherOperation *operation, guint source_id)
{
  nuvashalauncher_task_scope_forget_source (operation, source_id);
}
