/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-content-browser-model.h"

NuvashaContentBrowserModel *
nuvasha_content_browser_model_new (void)
{
  NuvashaContentBrowserModel *model = g_new0 (NuvashaContentBrowserModel, 1);

  model->more = g_cancellable_new ();
  model->cancellable = g_cancellable_new ();
  model->scope = nuvasha_task_scope_new (NULL, model->cancellable, NULL, NULL);
  model->seen_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           NULL);
  model->categories = g_ptr_array_new_with_free_func (g_free);
  model->projects = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           NULL);
  model->project_icons = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
  model->project_descriptions = g_hash_table_new_full (
    g_str_hash, g_str_equal, g_free, g_free);
  model->project_titles = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);

  return model;
}

void
nuvasha_content_browser_model_free (NuvashaContentBrowserModel *model)
{
  if (model == NULL)
    return;

  if (model->debounce_source != 0)
    nuvasha_task_scope_remove_source (model->scope, model->debounce_source);
  nuvasha_task_scope_unref (model->scope);
  g_cancellable_cancel (model->more);
  g_cancellable_cancel (model->cancellable);
  g_clear_object (&model->more);
  g_clear_object (&model->cancellable);
  g_clear_pointer (&model->seen_ids, g_hash_table_unref);
  g_clear_pointer (&model->projects, g_hash_table_unref);
  g_clear_pointer (&model->project_icons, g_hash_table_unref);
  g_clear_pointer (&model->project_descriptions, g_hash_table_unref);
  g_clear_pointer (&model->project_titles, g_hash_table_unref);
  g_clear_pointer (&model->categories, g_ptr_array_unref);
  g_clear_pointer (&model->results, g_ptr_array_unref);
  g_free (model->query);
  g_free (model);
}
