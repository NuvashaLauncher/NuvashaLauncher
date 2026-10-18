/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_MAIN_WINDOW_MODEL_H
#define comfy_MAIN_WINDOW_MODEL_H

#include "comfy-instance-manager.h"
#include "comfy-download.h"
#include "comfy-settings.h"
#include "comfy-users-popover.h"

typedef struct _ComfyMainWindowModel ComfyMainWindowModel;

ComfyMainWindowModel *comfy_main_window_model_new (void);
void comfy_main_window_model_free (ComfyMainWindowModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyMainWindowModel,
                               comfy_main_window_model_free)

struct _ComfyMainWindowModel
{
  ComfyInstanceManager *manager;
  ComfyDownloadService *download_service;
  ComfySettings *settings;
  ComfyAccounts *accounts;
  ComfyUsersPopover *users_popover;
  GHashTable *launchers;
  GHashTable *instance_dialogs;
};

#endif /* comfy_MAIN_WINDOW_MODEL_H */
