/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <glib.h>

#include "nuvasha-content-browser-model.h"
#include "nuvasha-content-install-model.h"

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
  g_autoptr (NuvashaContentBrowserModel) model =
    nuvasha_content_browser_model_new ();

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
  g_clear_pointer (&model, nuvasha_content_browser_model_free);
  g_assert_true (g_cancellable_is_cancelled (more));
  g_assert_true (g_cancellable_is_cancelled (cancellable));
}

static void
test_install_model (void)
{
  freed = 0;
  g_autoptr (NuvashaContentInstallModel) model =
    nuvasha_content_install_model_new (count_free);

  g_assert_nonnull (model->picks);
  g_assert_null (model->updates);
  g_assert_null (model->batch);
  g_assert_false (model->updating);
  g_assert_false (model->picking);
  g_assert_cmpuint (model->update_index, ==, 0);
  g_assert_cmpuint (model->pick_index, ==, 0);

  g_ptr_array_add (model->picks, g_strdup ("selected-project"));
  g_assert_cmpuint (model->picks->len, ==, 1);
  g_clear_pointer (&model->picks, g_ptr_array_unref);
  g_assert_cmpuint (freed, ==, 1);
}

int
main (void)
{
  test_browser_model ();
  test_install_model ();
  g_print ("ok - content browser and install models own their state\n");
  return 0;
}
