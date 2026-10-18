/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-main-window-model.h"

#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-launcher.h"

NuvashaMainWindowModel *
nuvasha_main_window_model_new (void)
{
  NuvashaMainWindowModel *model = g_new0 (NuvashaMainWindowModel, 1);

  model->settings = nuvasha_settings_new ();
  model->download_service = nuvasha_downloads_get_service ();
  nuvasha_download_set_cache_settings (model->download_service, model->settings);
  model->accounts = nuvasha_accounts_new ();
  model->users_popover = nuvasha_users_popover_new (model->accounts);
  g_object_ref_sink (model->users_popover);
  model->launchers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            g_object_unref);
  model->instance_dialogs = g_hash_table_new_full (
    g_str_hash, g_str_equal, g_free, NULL);

  return model;
}

void
nuvasha_main_window_model_free (NuvashaMainWindowModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->launchers, g_hash_table_unref);
  g_clear_pointer (&model->instance_dialogs, g_hash_table_unref);
  g_clear_object (&model->manager);
  nuvasha_download_service_cancel_queued (model->download_service);
  model->download_service = NULL;
  g_clear_object (&model->settings);
  g_clear_object (&model->accounts);
  g_clear_object (&model->users_popover);
  g_free (model);
}
