/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_MAIN_WINDOW_MODEL_H
#define AML_MAIN_WINDOW_MODEL_H

#include "aml-instance-manager.h"
#include "aml-settings.h"
#include "aml-users-popover.h"

typedef struct _AmlMainWindowModel AmlMainWindowModel;

AmlMainWindowModel *aml_main_window_model_new (void);
void aml_main_window_model_free (AmlMainWindowModel *model);

struct _AmlMainWindowModel
{
  AmlInstanceManager *manager;
  AmlSettings *settings;
  AmlAccounts *accounts;
  AmlUsersPopover *users_popover;
  GHashTable *launchers;
  GHashTable *instance_dialogs;
};

#endif /* AML_MAIN_WINDOW_MODEL_H */
