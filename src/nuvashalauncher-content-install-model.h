/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_CONTENT_INSTALL_MODEL_H
#define NUVASHALAUNCHER_CONTENT_INSTALL_MODEL_H

#include <glib.h>

typedef struct _NuvashaLauncherContentInstallModel NuvashaLauncherContentInstallModel;

NuvashaLauncherContentInstallModel *nuvashalauncher_content_install_model_new (GDestroyNotify pick_free);
void nuvashalauncher_content_install_model_free (NuvashaLauncherContentInstallModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherContentInstallModel,
                               nuvashalauncher_content_install_model_free)

struct _NuvashaLauncherContentInstallModel
{
  GDestroyNotify pick_free;
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
  GPtrArray *picks;   /* Pick* */
  GPtrArray *batch;   /* Pick*, the list being installed */
  gboolean picking;
  guint pick_index;
};

void nuvashalauncher_content_install_model_clear_picks (
  NuvashaLauncherContentInstallModel *model);
void nuvashalauncher_content_install_model_start_pick_batch (
  NuvashaLauncherContentInstallModel *model);
gpointer nuvashalauncher_content_install_model_next_pick (
  NuvashaLauncherContentInstallModel *model);
guint nuvashalauncher_content_install_model_finish_picks (
  NuvashaLauncherContentInstallModel *model);

void nuvashalauncher_content_install_model_start_updates (
  NuvashaLauncherContentInstallModel *model,
  GPtrArray                          *items);
gpointer nuvashalauncher_content_install_model_next_update (
  NuvashaLauncherContentInstallModel *model,
  guint                              *out_index,
  guint                              *out_total);
guint nuvashalauncher_content_install_model_finish_updates (
  NuvashaLauncherContentInstallModel *model);

#endif /* NUVASHALAUNCHER_CONTENT_INSTALL_MODEL_H */
