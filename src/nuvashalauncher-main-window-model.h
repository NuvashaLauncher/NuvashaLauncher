/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MAIN_WINDOW_MODEL_H
#define nuvashalauncher_MAIN_WINDOW_MODEL_H

#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-users-popover.h"

typedef struct _nuvashalauncherMainWindowModel nuvashalauncherMainWindowModel;

nuvashalauncherMainWindowModel *nuvashalauncher_main_window_model_new (void);
void nuvashalauncher_main_window_model_free (nuvashalauncherMainWindowModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherMainWindowModel,
                               nuvashalauncher_main_window_model_free)

struct _nuvashalauncherMainWindowModel
{
  nuvashalauncherInstanceManager *manager;
  nuvashalauncherDownloadService *download_service;
  nuvashalauncherSettings *settings;
  nuvashalauncherAccounts *accounts;
  nuvashalauncherUsersPopover *users_popover;
  GHashTable *launchers;
  GHashTable *instance_dialogs;
};

#endif /* nuvashalauncher_MAIN_WINDOW_MODEL_H */
