/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-ui-feedback.h"

void
aml_ui_show_toast (GtkWidget  *page,
                   GtkWidget  *overlay,
                   const char *message)
{
  AdwToast *toast = NULL;

  if (page == NULL || overlay == NULL ||
      gtk_widget_get_root (page) == NULL)
    return;

  toast = adw_toast_new (message);
  adw_toast_set_use_markup (toast, FALSE);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), toast);
}
