/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-ui-feedback.h"
#include "nuvasha-i18n.h"

GtkWidget *
nuvasha_ui_loading_child (void)
{
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  AdwSpinner *spinner = ADW_SPINNER (adw_spinner_new ());
  GtkLabel *label = GTK_LABEL (gtk_label_new (_("Loading…")));

  gtk_widget_set_valign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (spinner), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (spinner), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (label), "dim-label");
  gtk_box_append (box, GTK_WIDGET (spinner));
  gtk_box_append (box, GTK_WIDGET (label));

  return GTK_WIDGET (box);
}

void
nuvasha_ui_clear_error_label (GtkLabel *label)
{
  gtk_label_set_text (label, NULL);
  gtk_widget_set_visible (GTK_WIDGET (label), FALSE);
}

void
nuvasha_ui_cancel_dialog (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  adw_dialog_close (ADW_DIALOG (user_data));
}

void
nuvasha_ui_show_toast (GtkWidget  *page,
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
