/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MAIN_WINDOW_MODEL_H
#define nuvashalauncher_MAIN_WINDOW_MODEL_H

#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-users-popover.h"

typedef struct _NuvashaLauncherMainWindowModel NuvashaLauncherMainWindowModel;

NuvashaLauncherMainWindowModel *nuvashalauncher_main_window_model_new (void);
void nuvashalauncher_main_window_model_free (NuvashaLauncherMainWindowModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherMainWindowModel,
                               nuvashalauncher_main_window_model_free)

struct _NuvashaLauncherMainWindowModel
{
  NuvashaLauncherInstanceManager *manager;
  NuvashaLauncherDownloadService *download_service;
  NuvashaLauncherSettings *settings;
  NuvashaLauncherAccounts *accounts;
  NuvashaLauncherUsersPopover *users_popover;
  GHashTable *launchers;
  GHashTable *instance_dialogs;
};

#endif /* nuvashalauncher_MAIN_WINDOW_MODEL_H */
