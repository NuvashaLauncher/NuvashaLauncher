/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-install-model.h"

NuvashaLauncherContentInstallModel *
nuvashalauncher_content_install_model_new (void)
{
  return g_new0 (NuvashaLauncherContentInstallModel, 1);
}

void
nuvashalauncher_content_install_model_free (NuvashaLauncherContentInstallModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->updates, g_ptr_array_unref);
  g_free (model);
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
