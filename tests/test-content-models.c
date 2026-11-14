/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <glib.h>

#include "nuvashalauncher-content-browser-model.h"
#include "nuvashalauncher-content-install-model.h"

static guint freed;

static void
count_free (gpointer value)
{
  g_free (value);
  freed++;
}

static void
test_browser_model (void)
{
  g_autoptr (NuvashaLauncherContentBrowserModel) model =
    nuvashalauncher_content_browser_model_new ();

  g_assert_nonnull (model->projects);
  g_assert_nonnull (model->project_icons);
  g_assert_nonnull (model->project_descriptions);
  g_assert_nonnull (model->project_titles);
  g_assert_nonnull (model->categories);
  g_assert_nonnull (model->seen_ids);
  g_assert_nonnull (model->more);
  g_assert_nonnull (model->cancellable);
  g_assert_false (model->searched);
  g_assert_false (model->loading_page);
  g_assert_cmpuint (model->offset, ==, 0);

  g_autoptr (GCancellable) more = g_object_ref (model->more);
  g_autoptr (GCancellable) cancellable = g_object_ref (model->cancellable);

  /* Search & pagination state machine */
  nuvashalauncher_content_browser_model_set_query (model, "sodium");
  g_assert_cmpstr (model->query, ==, "sodium");

  GCancellable *first = nuvashalauncher_content_browser_model_begin_search (model, FALSE);
  g_assert_true (model->loading_page);
  g_assert_true (g_cancellable_is_cancelled (more));
  g_assert_false (g_cancellable_is_cancelled (first));

  g_assert_false (nuvashalauncher_content_browser_model_finish_page (model, 2));
  g_assert_false (model->loading_page);
  g_assert_cmpuint (model->total_hits, ==, 2);

  NuvashaLauncherModrinthProject proj = {
    .project_id = "AANobbMI",
    .title = "Sodium",
    .description = "Modern rendering engine",
    .icon_url = "https://cdn.modrinth.com/data/AANobbMI/icon.png",
  };
  g_assert_nonnull (nuvashalauncher_content_browser_model_accept_project (model, &proj));
  /* Duplicate project IDs across overlapping pages are rejected. */
  g_assert_null (nuvashalauncher_content_browser_model_accept_project (model, &proj));
  g_assert_cmpuint (model->results->len, ==, 1);
  g_assert_cmpstr (g_hash_table_lookup (model->project_titles, "AANobbMI"), ==, "Sodium");

  nuvashalauncher_content_browser_model_advance_offset (model, 1);
  g_assert_true (nuvashalauncher_content_browser_model_has_more (model));
  nuvashalauncher_content_browser_model_advance_offset (model, 1);
  g_assert_false (nuvashalauncher_content_browser_model_has_more (model));

  nuvashalauncher_content_browser_model_begin_search (model, TRUE);
  g_assert_true (nuvashalauncher_content_browser_model_fail_page (model));
  g_assert_false (model->loading_page);
  g_assert_nonnull (model->results);

  GCancellable *next_cancellable =
    nuvashalauncher_content_browser_model_reset_cancellable (model);
  g_assert_true (g_cancellable_is_cancelled (cancellable));
  g_assert_false (g_cancellable_is_cancelled (next_cancellable));

  g_clear_pointer (&model, nuvashalauncher_content_browser_model_free);
  g_assert_true (g_cancellable_is_cancelled (first));
  g_assert_true (g_cancellable_is_cancelled (next_cancellable));
}

static void
test_install_model (void)
{
  freed = 0;
  g_autoptr (NuvashaLauncherContentInstallModel) model =
    nuvashalauncher_content_install_model_new (count_free);

  g_assert_nonnull (model->picks);
  g_assert_null (model->updates);
  g_assert_null (model->batch);
  g_assert_false (model->updating);
  g_assert_false (model->picking);
  g_assert_cmpuint (model->update_index, ==, 0);
  g_assert_cmpuint (model->pick_index, ==, 0);

  g_ptr_array_add (model->picks, g_strdup ("selected-project"));
  g_ptr_array_add (model->picks, g_strdup ("second-project"));
  g_assert_cmpuint (model->picks->len, ==, 2);

  nuvashalauncher_content_install_model_start_pick_batch (model);
  g_assert_true (model->picking);
  g_assert_cmpuint (model->picks->len, ==, 0);
  g_assert_cmpstr (nuvashalauncher_content_install_model_next_pick (model), ==, "selected-project");
  g_assert_cmpstr (nuvashalauncher_content_install_model_next_pick (model), ==, "second-project");
  g_assert_null (nuvashalauncher_content_install_model_next_pick (model));
  g_assert_cmpuint (nuvashalauncher_content_install_model_finish_picks (model), ==, 2);
  g_assert_false (model->picking);
  g_assert_cmpuint (freed, ==, 2);

  GPtrArray *updates = g_ptr_array_new_with_free_func (count_free);
  g_ptr_array_add (updates, g_strdup ("update-a"));
  nuvashalauncher_content_install_model_start_updates (model, updates);
  g_assert_true (model->updating);

  guint idx = 99;
  guint total = 0;
  g_assert_cmpstr (
    nuvashalauncher_content_install_model_next_update (model, &idx, &total), ==, "update-a");
  g_assert_cmpuint (idx, ==, 0);
  g_assert_cmpuint (total, ==, 1);
  g_assert_null (nuvashalauncher_content_install_model_next_update (model, &idx, &total));
  g_assert_cmpuint (nuvashalauncher_content_install_model_finish_updates (model), ==, 1);
  g_assert_false (model->updating);
  g_assert_cmpuint (freed, ==, 3);
}

int
main (void)
{
  test_browser_model ();
  test_install_model ();
  g_print ("ok - content browser and install models own their state\n");
  return 0;
}
