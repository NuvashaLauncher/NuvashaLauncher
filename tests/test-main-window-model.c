/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <gtk/gtk.h>

#include "aml-main-window-model.h"

static void
test_model_owns_window_services (void)
{
  g_autoptr (AmlMainWindowModel) model = NULL;

  if (!gtk_init_check ())
    {
      g_print ("skipped - no display for main window model test\n");
      return;
    }

  model = aml_main_window_model_new ();
  g_assert_nonnull (model->settings);
  g_assert_nonnull (model->accounts);
  g_assert_nonnull (model->users_popover);
  g_assert_nonnull (model->launchers);
  g_assert_nonnull (model->instance_dialogs);
  g_assert_null (model->manager);
  g_print ("ok - main window model owns shared services and state\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Main window model\n");
  test_model_owns_window_services ();
  return 0;
}
