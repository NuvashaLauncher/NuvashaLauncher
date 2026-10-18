/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_MAIN_WINDOW_MODEL_H
#define nuvasha_MAIN_WINDOW_MODEL_H

#include "nuvasha-instance-manager.h"
#include "nuvasha-download.h"
#include "nuvasha-settings.h"
#include "nuvasha-users-popover.h"

typedef struct _NuvashaMainWindowModel NuvashaMainWindowModel;

NuvashaMainWindowModel *nuvasha_main_window_model_new (void);
void nuvasha_main_window_model_free (NuvashaMainWindowModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaMainWindowModel,
                               nuvasha_main_window_model_free)

struct _NuvashaMainWindowModel
{
  NuvashaInstanceManager *manager;
  NuvashaDownloadService *download_service;
  NuvashaSettings *settings;
  NuvashaAccounts *accounts;
  NuvashaUsersPopover *users_popover;
  GHashTable *launchers;
  GHashTable *instance_dialogs;
};

#endif /* nuvasha_MAIN_WINDOW_MODEL_H */
