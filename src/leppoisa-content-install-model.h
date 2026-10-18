/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_CONTENT_INSTALL_MODEL_H
#define leppoisa_CONTENT_INSTALL_MODEL_H

#include <glib.h>

typedef struct _LeppoisaContentInstallModel LeppoisaContentInstallModel;

LeppoisaContentInstallModel *leppoisa_content_install_model_new (GDestroyNotify pick_free);
void leppoisa_content_install_model_free (LeppoisaContentInstallModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LeppoisaContentInstallModel,
                               leppoisa_content_install_model_free)

struct _LeppoisaContentInstallModel
{
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
  GPtrArray *picks;   /* Pick* */
  GPtrArray *batch;   /* Pick*, the list being installed */
  gboolean picking;
  guint pick_index;
};

#endif /* leppoisa_CONTENT_INSTALL_MODEL_H */
