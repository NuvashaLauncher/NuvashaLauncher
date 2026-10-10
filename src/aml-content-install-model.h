/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_CONTENT_INSTALL_MODEL_H
#define AML_CONTENT_INSTALL_MODEL_H

#include <glib.h>

typedef struct _AmlContentInstallModel AmlContentInstallModel;

AmlContentInstallModel *aml_content_install_model_new (GDestroyNotify pick_free);
void aml_content_install_model_free (AmlContentInstallModel *model);

struct _AmlContentInstallModel
{
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
  GPtrArray *picks;   /* Pick* */
  GPtrArray *batch;   /* Pick*, the list being installed */
  gboolean picking;
  guint pick_index;
};

#endif /* AML_CONTENT_INSTALL_MODEL_H */
