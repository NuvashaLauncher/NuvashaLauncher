/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-content-install-model.h"

ComfyContentInstallModel *
comfy_content_install_model_new (GDestroyNotify pick_free)
{
  ComfyContentInstallModel *model = g_new0 (ComfyContentInstallModel, 1);

  model->picks = g_ptr_array_new_with_free_func (pick_free);
  return model;
}

void
comfy_content_install_model_free (ComfyContentInstallModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->updates, g_ptr_array_unref);
  g_clear_pointer (&model->picks, g_ptr_array_unref);
  g_clear_pointer (&model->batch, g_ptr_array_unref);
  g_free (model);
}
