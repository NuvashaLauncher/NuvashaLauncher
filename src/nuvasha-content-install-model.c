/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-content-install-model.h"

NuvashaContentInstallModel *
nuvasha_content_install_model_new (GDestroyNotify pick_free)
{
  NuvashaContentInstallModel *model = g_new0 (NuvashaContentInstallModel, 1);

  model->picks = g_ptr_array_new_with_free_func (pick_free);
  return model;
}

void
nuvasha_content_install_model_free (NuvashaContentInstallModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->updates, g_ptr_array_unref);
  g_clear_pointer (&model->picks, g_ptr_array_unref);
  g_clear_pointer (&model->batch, g_ptr_array_unref);
  g_free (model);
}
