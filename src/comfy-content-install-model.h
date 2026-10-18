/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_CONTENT_INSTALL_MODEL_H
#define comfy_CONTENT_INSTALL_MODEL_H

#include <glib.h>

typedef struct _ComfyContentInstallModel ComfyContentInstallModel;

ComfyContentInstallModel *comfy_content_install_model_new (GDestroyNotify pick_free);
void comfy_content_install_model_free (ComfyContentInstallModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyContentInstallModel,
                               comfy_content_install_model_free)

struct _ComfyContentInstallModel
{
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
  GPtrArray *picks;   /* Pick* */
  GPtrArray *batch;   /* Pick*, the list being installed */
  gboolean picking;
  guint pick_index;
};

#endif /* comfy_CONTENT_INSTALL_MODEL_H */
