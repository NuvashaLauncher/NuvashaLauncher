/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_MAIN_WINDOW_MODEL_H
#define leppoisa_MAIN_WINDOW_MODEL_H

#include "leppoisa-instance-manager.h"
#include "leppoisa-download.h"
#include "leppoisa-settings.h"
#include "leppoisa-users-popover.h"

typedef struct _LeppoisaMainWindowModel LeppoisaMainWindowModel;

LeppoisaMainWindowModel *leppoisa_main_window_model_new (void);
void leppoisa_main_window_model_free (LeppoisaMainWindowModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LeppoisaMainWindowModel,
                               leppoisa_main_window_model_free)

struct _LeppoisaMainWindowModel
{
  LeppoisaInstanceManager *manager;
  LeppoisaDownloadService *download_service;
  LeppoisaSettings *settings;
  LeppoisaAccounts *accounts;
  LeppoisaUsersPopover *users_popover;
  GHashTable *launchers;
  GHashTable *instance_dialogs;
};

#endif /* leppoisa_MAIN_WINDOW_MODEL_H */
