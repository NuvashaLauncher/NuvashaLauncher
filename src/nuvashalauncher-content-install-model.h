/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_CONTENT_INSTALL_MODEL_H
#define nuvashalauncher_CONTENT_INSTALL_MODEL_H

#include <glib.h>

typedef struct _nuvashalauncherContentInstallModel nuvashalauncherContentInstallModel;

nuvashalauncherContentInstallModel *nuvashalauncher_content_install_model_new (GDestroyNotify pick_free);
void nuvashalauncher_content_install_model_free (nuvashalauncherContentInstallModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherContentInstallModel,
                               nuvashalauncher_content_install_model_free)

struct _nuvashalauncherContentInstallModel
{
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
  GPtrArray *picks;   /* Pick* */
  GPtrArray *batch;   /* Pick*, the list being installed */
  gboolean picking;
  guint pick_index;
};

#endif /* nuvashalauncher_CONTENT_INSTALL_MODEL_H */
