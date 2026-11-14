/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-browser-model.h"

NuvashaLauncherContentBrowserModel *
nuvashalauncher_content_browser_model_new (void)
{
  NuvashaLauncherContentBrowserModel *model = g_new0 (NuvashaLauncherContentBrowserModel, 1);

  model->more = g_cancellable_new ();
  model->cancellable = g_cancellable_new ();
  model->scope = nuvashalauncher_task_scope_new (NULL, model->cancellable, NULL, NULL);
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
nuvashalauncher_content_browser_model_free (NuvashaLauncherContentBrowserModel *model)
{
  if (model == NULL)
    return;

  if (model->debounce_source != 0)
    nuvashalauncher_task_scope_remove_source (model->scope, model->debounce_source);
  nuvashalauncher_task_scope_unref (model->scope);
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

GCancellable *
nuvashalauncher_content_browser_model_begin_search (
  NuvashaLauncherContentBrowserModel *model,
  gboolean                            append)
{
  g_return_val_if_fail (model != NULL, NULL);

  if (!append)
    {
      g_cancellable_cancel (model->more);
      g_clear_object (&model->more);
      model->more = g_cancellable_new ();
      model->offset = 0;
    }

  model->loading_page = TRUE;
  model->pending_append = append;
  return model->more;
}

gboolean
nuvashalauncher_content_browser_model_finish_page (
  NuvashaLauncherContentBrowserModel *model,
  guint64                             total_hits)
{
  gboolean append;

  g_return_val_if_fail (model != NULL, FALSE);

  append = model->pending_append;
  model->loading_page = FALSE;

  if (!append)
    {
      g_clear_pointer (&model->results, g_ptr_array_unref);
      model->results = g_ptr_array_new_with_free_func (
        (GDestroyNotify) nuvashalauncher_modrinth_project_free);
      model->total_hits = total_hits;
      g_hash_table_remove_all (model->seen_ids);
    }

  return append;
}

gboolean
nuvashalauncher_content_browser_model_fail_page (
  NuvashaLauncherContentBrowserModel *model)
{
  gboolean append;

  g_return_val_if_fail (model != NULL, FALSE);

  append = model->pending_append;
  model->loading_page = FALSE;

  if (!append)
    g_clear_pointer (&model->results, g_ptr_array_unref);

  return append;
}

const NuvashaLauncherModrinthProject *
nuvashalauncher_content_browser_model_accept_project (
  NuvashaLauncherContentBrowserModel *model,
  NuvashaLauncherModrinthProject     *project)
{
  NuvashaLauncherModrinthProject *copy = NULL;

  g_return_val_if_fail (model != NULL, NULL);
  g_return_val_if_fail (project != NULL, NULL);

  if (project->project_id == NULL ||
      g_hash_table_contains (model->seen_ids, project->project_id))
    return NULL;

  g_hash_table_add (model->seen_ids, g_strdup (project->project_id));

  if (project->icon_url != NULL && *project->icon_url != '\0')
    g_hash_table_insert (model->project_icons,
                         g_strdup (project->project_id),
                         g_strdup (project->icon_url));
  if (project->description != NULL && *project->description != '\0')
    g_hash_table_insert (model->project_descriptions,
                         g_strdup (project->project_id),
                         g_strdup (project->description));
  if (project->title != NULL && *project->title != '\0')
    g_hash_table_insert (model->project_titles,
                         g_strdup (project->project_id),
                         g_strdup (project->title));

  if (model->results == NULL)
    model->results = g_ptr_array_new_with_free_func (
      (GDestroyNotify) nuvashalauncher_modrinth_project_free);

  copy = nuvashalauncher_modrinth_project_copy (project);
  g_ptr_array_add (model->results, copy);
  return copy;
}

void
nuvashalauncher_content_browser_model_advance_offset (
  NuvashaLauncherContentBrowserModel *model,
  guint                               shown)
{
  g_return_if_fail (model != NULL);
  model->offset += shown;
}

gboolean
nuvashalauncher_content_browser_model_has_more (
  const NuvashaLauncherContentBrowserModel *model)
{
  g_return_val_if_fail (model != NULL, FALSE);
  return model->offset < model->total_hits;
}

void
nuvashalauncher_content_browser_model_set_query (
  NuvashaLauncherContentBrowserModel *model,
  const char                         *query)
{
  g_return_if_fail (model != NULL);

  g_free (model->query);
  model->query = query != NULL ? g_strdup (query) : g_strdup ("");
  model->offset = 0;
}

GCancellable *
nuvashalauncher_content_browser_model_reset_cancellable (
  NuvashaLauncherContentBrowserModel *model)
{
  g_return_val_if_fail (model != NULL, NULL);

  g_cancellable_cancel (model->cancellable);
  g_clear_object (&model->cancellable);
  model->cancellable = g_cancellable_new ();
  return model->cancellable;
}
