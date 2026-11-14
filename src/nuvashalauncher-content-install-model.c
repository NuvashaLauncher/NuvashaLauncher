/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-install-model.h"

NuvashaLauncherContentInstallModel *
nuvashalauncher_content_install_model_new (GDestroyNotify pick_free)
{
  NuvashaLauncherContentInstallModel *model = g_new0 (NuvashaLauncherContentInstallModel, 1);

  model->pick_free = pick_free;
  model->picks = g_ptr_array_new_with_free_func (pick_free);
  return model;
}

void
nuvashalauncher_content_install_model_free (NuvashaLauncherContentInstallModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->updates, g_ptr_array_unref);
  g_clear_pointer (&model->picks, g_ptr_array_unref);
  g_clear_pointer (&model->batch, g_ptr_array_unref);
  g_free (model);
}

void
nuvashalauncher_content_install_model_clear_picks (
  NuvashaLauncherContentInstallModel *model)
{
  g_return_if_fail (model != NULL);

  g_clear_pointer (&model->picks, g_ptr_array_unref);
  model->picks = g_ptr_array_new_with_free_func (model->pick_free);
}

void
nuvashalauncher_content_install_model_start_pick_batch (
  NuvashaLauncherContentInstallModel *model)
{
  g_return_if_fail (model != NULL);

  g_clear_pointer (&model->batch, g_ptr_array_unref);
  model->batch = g_steal_pointer (&model->picks);
  model->picks = g_ptr_array_new_with_free_func (model->pick_free);
  model->picking = TRUE;
  model->pick_index = 0;
}

gpointer
nuvashalauncher_content_install_model_next_pick (
  NuvashaLauncherContentInstallModel *model)
{
  g_return_val_if_fail (model != NULL, NULL);

  if (model->batch == NULL || model->pick_index >= model->batch->len)
    return NULL;

  return g_ptr_array_index (model->batch, model->pick_index++);
}

guint
nuvashalauncher_content_install_model_finish_picks (
  NuvashaLauncherContentInstallModel *model)
{
  guint done;

  g_return_val_if_fail (model != NULL, 0);

  done = model->batch != NULL ? model->batch->len : 0;
  g_clear_pointer (&model->batch, g_ptr_array_unref);
  model->picking = FALSE;
  model->pick_index = 0;
  return done;
}

void
nuvashalauncher_content_install_model_start_updates (
  NuvashaLauncherContentInstallModel *model,
  GPtrArray                          *items)
{
  g_return_if_fail (model != NULL);

  g_clear_pointer (&model->updates, g_ptr_array_unref);
  model->updates = items;
  model->update_index = 0;
  model->updating = TRUE;
}

gpointer
nuvashalauncher_content_install_model_next_update (
  NuvashaLauncherContentInstallModel *model,
  guint                              *out_index,
  guint                              *out_total)
{
  gpointer item;

  g_return_val_if_fail (model != NULL, NULL);

  if (model->updates == NULL || model->update_index >= model->updates->len)
    return NULL;

  if (out_index != NULL)
    *out_index = model->update_index;
  if (out_total != NULL)
    *out_total = model->updates->len;

  item = g_ptr_array_index (model->updates, model->update_index);
  model->update_index++;
  return item;
}

guint
nuvashalauncher_content_install_model_finish_updates (
  NuvashaLauncherContentInstallModel *model)
{
  guint done;

  g_return_val_if_fail (model != NULL, 0);

  done = model->updates != NULL ? model->updates->len : 0;
  g_clear_pointer (&model->updates, g_ptr_array_unref);
  model->update_index = 0;
  model->updating = FALSE;
  return done;
}
