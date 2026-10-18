/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-content-install-model.h"

LeppoisaContentInstallModel *
leppoisa_content_install_model_new (GDestroyNotify pick_free)
{
  LeppoisaContentInstallModel *model = g_new0 (LeppoisaContentInstallModel, 1);

  model->picks = g_ptr_array_new_with_free_func (pick_free);
  return model;
}

void
leppoisa_content_install_model_free (LeppoisaContentInstallModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->updates, g_ptr_array_unref);
  g_clear_pointer (&model->picks, g_ptr_array_unref);
  g_clear_pointer (&model->batch, g_ptr_array_unref);
  g_free (model);
}
