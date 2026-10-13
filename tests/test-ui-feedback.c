/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <adwaita.h>
#include <glib.h>

#include "aml-ui-feedback.h"

static void
test_feedback_ignores_unattached_pages (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = adw_toast_overlay_new ();

  /* Missing targets must be harmless before GTK has a root/window. */
  aml_ui_show_toast (NULL, NULL, "message");
  aml_ui_show_toast (page, overlay, "message");
  g_assert_null (gtk_widget_get_root (page));
  g_assert_nonnull (overlay);
  g_object_ref_sink (page);
  g_object_unref (page);
  g_object_ref_sink (overlay);
  g_object_unref (overlay);
  g_print ("ok - feedback ignores unattached or incomplete toast targets\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# UI feedback\n");
  if (!gtk_init_check ())
    {
      g_print ("skipped - no display for toast feedback test\n");
      return 77;
    }
  test_feedback_ignores_unattached_pages ();
  return 0;
}
