/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_CONTENT_INSTALL_MODEL_H
#define nuvasha_CONTENT_INSTALL_MODEL_H

#include <glib.h>

typedef struct _NuvashaContentInstallModel NuvashaContentInstallModel;

NuvashaContentInstallModel *nuvasha_content_install_model_new (GDestroyNotify pick_free);
void nuvasha_content_install_model_free (NuvashaContentInstallModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaContentInstallModel,
                               nuvasha_content_install_model_free)

struct _NuvashaContentInstallModel
{
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
  GPtrArray *picks;   /* Pick* */
  GPtrArray *batch;   /* Pick*, the list being installed */
  gboolean picking;
  guint pick_index;
};

#endif /* nuvasha_CONTENT_INSTALL_MODEL_H */
