/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <glib.h>

#include "nuvasha-content-browser-model.h"

static void
test_model_owns_browser_state (void)
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
  g_print ("ok - browser model owns initialized search state\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Content browser model\n");
  test_model_owns_browser_state ();
  return 0;
}
