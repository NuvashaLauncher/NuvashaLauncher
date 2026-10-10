/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <adwaita.h>
#include <glib.h>

#include "aml-ui-feedback.h"

static void
test_feedback_ignores_unattached_pages (void)
{
  /* Missing targets must be harmless before GTK has a root/window. */
  aml_ui_show_toast (NULL, NULL, "message");
  g_print ("ok - feedback ignores unattached or incomplete toast targets\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# UI feedback\n");
  test_feedback_ignores_unattached_pages ();
  return 0;
}
