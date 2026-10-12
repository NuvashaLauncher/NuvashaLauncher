/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-main-window-model.h"

#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-launcher.h"

AmlMainWindowModel *
aml_main_window_model_new (void)
{
  AmlMainWindowModel *model = g_new0 (AmlMainWindowModel, 1);

  model->settings = aml_settings_new ();
  model->download_service = aml_downloads_get_service ();
  aml_download_set_cache_settings (model->download_service, model->settings);
  model->accounts = aml_accounts_new ();
  model->users_popover = aml_users_popover_new (model->accounts);
  g_object_ref_sink (model->users_popover);
  model->launchers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            g_object_unref);
  model->instance_dialogs = g_hash_table_new_full (
    g_str_hash, g_str_equal, g_free, NULL);

  return model;
}

void
aml_main_window_model_free (AmlMainWindowModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->launchers, g_hash_table_unref);
  g_clear_pointer (&model->instance_dialogs, g_hash_table_unref);
  g_clear_object (&model->manager);
  aml_download_service_cancel_queued (model->download_service);
  model->download_service = NULL;
  g_clear_object (&model->settings);
  g_clear_object (&model->accounts);
  g_clear_object (&model->users_popover);
  g_free (model);
}
