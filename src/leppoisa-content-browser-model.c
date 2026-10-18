/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-content-browser-model.h"

LeppoisaContentBrowserModel *
leppoisa_content_browser_model_new (void)
{
  LeppoisaContentBrowserModel *model = g_new0 (LeppoisaContentBrowserModel, 1);

  model->more = g_cancellable_new ();
  model->cancellable = g_cancellable_new ();
  model->scope = leppoisa_task_scope_new (NULL, model->cancellable, NULL, NULL);
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
leppoisa_content_browser_model_free (LeppoisaContentBrowserModel *model)
{
  if (model == NULL)
    return;

  if (model->debounce_source != 0)
    leppoisa_task_scope_remove_source (model->scope, model->debounce_source);
  leppoisa_task_scope_unref (model->scope);
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
