/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_CONTENT_INSTALL_MODEL_H
#define NUVASHALAUNCHER_CONTENT_INSTALL_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NuvashaLauncherContentInstallModel NuvashaLauncherContentInstallModel;

NuvashaLauncherContentInstallModel *nuvashalauncher_content_install_model_new (void);
void nuvashalauncher_content_install_model_free (NuvashaLauncherContentInstallModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherContentInstallModel,
                               nuvashalauncher_content_install_model_free)

struct _NuvashaLauncherContentInstallModel
{
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
};

void nuvashalauncher_content_install_model_start_updates (
  NuvashaLauncherContentInstallModel *model,
  GPtrArray                          *items);
gpointer nuvashalauncher_content_install_model_next_update (
  NuvashaLauncherContentInstallModel *model,
  guint                              *out_index,
  guint                              *out_total);
guint nuvashalauncher_content_install_model_finish_updates (
  NuvashaLauncherContentInstallModel *model);

G_END_DECLS

#endif /* NUVASHALAUNCHER_CONTENT_INSTALL_MODEL_H */
