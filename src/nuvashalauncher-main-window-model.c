/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-model.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-launcher.h"

NuvashaLauncherMainWindowModel *
nuvashalauncher_main_window_model_new (void)
{
  NuvashaLauncherMainWindowModel *model = g_new0 (NuvashaLauncherMainWindowModel, 1);

  model->settings = nuvashalauncher_settings_new ();
  model->download_service = nuvashalauncher_downloads_get_service ();
  nuvashalauncher_download_set_cache_settings (model->download_service, model->settings);
  model->accounts = nuvashalauncher_accounts_new ();
  model->launchers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            g_object_unref);
  model->instance_dialogs = g_hash_table_new_full (
    g_str_hash, g_str_equal, g_free, NULL);

  return model;
}

void
nuvashalauncher_main_window_model_free (NuvashaLauncherMainWindowModel *model)
{
  if (model == NULL)
    return;

  g_clear_pointer (&model->launchers, g_hash_table_unref);
  g_clear_pointer (&model->instance_dialogs, g_hash_table_unref);
  g_clear_object (&model->manager);
  nuvashalauncher_download_service_cancel_queued (model->download_service);
  model->download_service = NULL;
  g_clear_object (&model->settings);
  g_clear_object (&model->accounts);
  g_free (model);
}

gboolean
nuvashalauncher_main_window_model_has_active_launch (NuvashaLauncherMainWindowModel *model)
{
  GHashTableIter iter;
  gpointer value = NULL;

  if (model == NULL || model->launchers == NULL)
    return FALSE;

  g_hash_table_iter_init (&iter, model->launchers);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (nuvashalauncher_launch_state_is_busy (
          nuvashalauncher_launcher_get_state (NUVASHALAUNCHER_LAUNCHER (value))))
      return TRUE;

  return FALSE;
}

NuvashaLauncher *
nuvashalauncher_main_window_model_get_or_create_launcher (NuvashaLauncherMainWindowModel *model,
                                                          const char                     *instance_id,
                                                          gboolean                       *created)
{
  NuvashaLauncher *launcher = NULL;
  NuvashaLauncherInstance *instance = NULL;

  if (created != NULL)
    *created = FALSE;

  if (model == NULL || instance_id == NULL)
    return NULL;

  launcher = g_hash_table_lookup (model->launchers, instance_id);
  if (launcher != NULL)
    return launcher;

  if (model->manager == NULL)
    return NULL;

  instance = nuvashalauncher_instance_manager_get_by_id (model->manager, instance_id);
  if (instance == NULL)
    return NULL;

  launcher = nuvashalauncher_launcher_new (instance, model->settings, model->accounts);
  g_hash_table_insert (model->launchers, g_strdup (instance_id), launcher);

  if (created != NULL)
    *created = TRUE;

  return launcher;
}
