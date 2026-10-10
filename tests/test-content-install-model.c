/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <glib.h>

#include "aml-content-install-model.h"

static void
test_model_owns_install_state (void)
{
  g_autoptr (AmlContentInstallModel) model =
    aml_content_install_model_new (g_free);

  g_assert_nonnull (model->picks);
  g_assert_null (model->updates);
  g_assert_null (model->batch);
  g_assert_false (model->updating);
  g_assert_false (model->picking);
  g_assert_cmpuint (model->update_index, ==, 0);
  g_assert_cmpuint (model->pick_index, ==, 0);

  g_ptr_array_add (model->picks, g_strdup ("selected-project"));
  g_assert_cmpuint (model->picks->len, ==, 1);
  g_print ("ok - install model owns selection state and defaults\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Content install model\n");
  test_model_owns_install_state ();
  return 0;
}
