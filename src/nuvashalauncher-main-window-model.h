/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_MAIN_WINDOW_MODEL_H
#define NUVASHALAUNCHER_MAIN_WINDOW_MODEL_H

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-settings.h"

G_BEGIN_DECLS

typedef struct _NuvashaLauncherMainWindowModel NuvashaLauncherMainWindowModel;

NuvashaLauncherMainWindowModel *nuvashalauncher_main_window_model_new (void);
void nuvashalauncher_main_window_model_free (NuvashaLauncherMainWindowModel *model);
gboolean nuvashalauncher_main_window_model_has_active_launch (NuvashaLauncherMainWindowModel *model);
NuvashaLauncher *nuvashalauncher_main_window_model_get_or_create_launcher (
  NuvashaLauncherMainWindowModel *model,
  const char                     *instance_id,
  gboolean                       *created);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherMainWindowModel,
                               nuvashalauncher_main_window_model_free)

struct _NuvashaLauncherMainWindowModel
{
  NuvashaLauncherInstanceManager *manager;
  NuvashaLauncherDownloadService *download_service;
  NuvashaLauncherSettings *settings;
  NuvashaLauncherAccounts *accounts;
  GHashTable *launchers;
  GHashTable *instance_dialogs;
};

G_END_DECLS

#endif /* NUVASHALAUNCHER_MAIN_WINDOW_MODEL_H */
