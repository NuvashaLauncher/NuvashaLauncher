/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-i18n.h"

void
nuvashalauncher_ui_ensure_icons (void)
{
  static gsize initialized = 0;
  if (g_once_init_enter (&initialized))
    {
      GdkDisplay *display = gdk_display_get_default ();
      if (display != NULL)
        {
          GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display (display);
          gtk_icon_theme_add_resource_path (
            icon_theme, "/com/github/NuvashaLauncher/NuvashaLauncher/icons");
        }
      g_once_init_leave (&initialized, 1);
    }
}

GtkWidget *
nuvashalauncher_ui_loading_child (void)
{
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  AdwSpinner *spinner = ADW_SPINNER (adw_spinner_new ());

  gtk_widget_set_valign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (box), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (box), TRUE);
  gtk_widget_set_size_request (GTK_WIDGET (spinner), 64, 64);
  gtk_widget_set_valign (GTK_WIDGET (spinner), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (spinner), GTK_ALIGN_CENTER);
  gtk_box_append (box, GTK_WIDGET (spinner));

  return GTK_WIDGET (box);
}

void
nuvashalauncher_ui_clear_error_label (GtkLabel *label)
{
  gtk_label_set_text (label, NULL);
  gtk_widget_set_tooltip_text (GTK_WIDGET (label), NULL);
  gtk_widget_set_visible (GTK_WIDGET (label), FALSE);
}

GtkWidget *
nuvashalauncher_ui_error_details (const GError *error)
{
  GtkWidget *details = gtk_label_new (nuvashalauncher_error_message (error));
  GtkWidget *expander = gtk_expander_new (_("Technical Details"));
  gtk_label_set_use_markup (GTK_LABEL (details), FALSE);
  gtk_label_set_wrap (GTK_LABEL (details), TRUE);
  gtk_label_set_selectable (GTK_LABEL (details), TRUE);
  gtk_label_set_xalign (GTK_LABEL (details), 0);
  gtk_expander_set_child (GTK_EXPANDER (expander), details);
  return expander;
}

void
nuvashalauncher_ui_show_error (GtkWidget *parent, const char *title,
                              const GError *error)
{
  AdwAlertDialog *dialog;

  if (nuvashalauncher_error_is_cancelled (error))
    return;
  if (parent == NULL || gtk_widget_get_root (parent) == NULL)
    {
      GApplication *app = g_application_get_default ();
      parent = GTK_IS_APPLICATION (app)
        ? GTK_WIDGET (gtk_application_get_active_window (GTK_APPLICATION (app))) : NULL;
    }
  if (parent == NULL)
    return;

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (
    title, error != NULL ? nuvashalauncher_error_message (error) : NULL));
  adw_alert_dialog_set_heading_use_markup (dialog, FALSE);
  adw_alert_dialog_set_body_use_markup (dialog, FALSE);
  if (error != NULL)
    adw_alert_dialog_set_extra_child (dialog, nuvashalauncher_ui_error_details (error));
  adw_alert_dialog_add_response (dialog, "ok", _("OK"));
  adw_alert_dialog_set_close_response (dialog, "ok");
  adw_dialog_present (ADW_DIALOG (dialog), parent);
}

void
nuvashalauncher_ui_error_label (GtkLabel *label, const GError *error)
{
  gtk_label_set_text (label, nuvashalauncher_error_message (error));
  /* Inline errors keep the original diagnostic available on hover. */
  gtk_widget_set_tooltip_text (GTK_WIDGET (label),
                               error != NULL ? nuvashalauncher_error_message (error) : NULL);
  gtk_widget_set_visible (GTK_WIDGET (label), TRUE);
}

static void
on_error_banner_details (AdwBanner *banner, gpointer user_data G_GNUC_UNUSED)
{
  const GError *error = g_object_get_data (G_OBJECT (banner), "nuvashalauncher-error");
  nuvashalauncher_ui_show_error (GTK_WIDGET (banner),
                                adw_banner_get_title (banner), error);
  adw_banner_set_revealed (banner, FALSE);
}

void
nuvashalauncher_ui_error_banner (AdwBanner *banner, const GError *error)
{
  if (nuvashalauncher_error_is_cancelled (error))
    return;
  g_object_set_data_full (G_OBJECT (banner), "nuvashalauncher-error",
                          error != NULL ? g_error_copy (error) : NULL,
                          (GDestroyNotify) g_error_free);
  adw_banner_set_use_markup (banner, FALSE);
  adw_banner_set_title (banner, nuvashalauncher_error_message (error));
  adw_banner_set_button_label (banner, _("Technical Details"));
  if (g_object_get_data (G_OBJECT (banner), "nuvashalauncher-error-handler") == NULL)
    {
      g_signal_connect (banner, "button-clicked", G_CALLBACK (on_error_banner_details), NULL);
      g_object_set_data (G_OBJECT (banner), "nuvashalauncher-error-handler", GINT_TO_POINTER (1));
    }
  adw_banner_set_revealed (banner, TRUE);
}

typedef struct {
  GWeakRef parent;
  GError *error;
} ErrorToast;

static void
error_toast_free (gpointer data)
{
  ErrorToast *details = data;
  g_weak_ref_clear (&details->parent);
  g_clear_error (&details->error);
  g_free (details);
}

static void
on_error_toast_details (AdwToast *toast G_GNUC_UNUSED, gpointer user_data)
{
  ErrorToast *details = user_data;
  g_autoptr (GtkWidget) parent = g_weak_ref_get (&details->parent);
  nuvashalauncher_ui_show_error (parent, _("Technical Details"), details->error);
}

void
nuvashalauncher_ui_error_toast (GtkWidget *page, GtkWidget *overlay, const GError *error)
{
  AdwToast *toast;
  ErrorToast *details;

  if (nuvashalauncher_error_is_cancelled (error))
    return;
  if (page == NULL || overlay == NULL || gtk_widget_get_root (page) == NULL)
    {
      nuvashalauncher_ui_show_error (page, _("Technical Details"), error);
      return;
    }
  details = g_new0 (ErrorToast, 1);
  g_weak_ref_init (&details->parent, page);
  details->error = error != NULL ? g_error_copy (error) : NULL;
  toast = adw_toast_new (nuvashalauncher_error_message (error));
  adw_toast_set_use_markup (toast, FALSE);
  adw_toast_set_timeout (toast, 0);
  adw_toast_set_button_label (toast, _("Technical Details"));
  g_object_set_data_full (G_OBJECT (toast), "nuvashalauncher-error", details, error_toast_free);
  g_signal_connect (toast, "button-clicked", G_CALLBACK (on_error_toast_details), details);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), toast);
}

void
nuvashalauncher_ui_cancel_dialog (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  adw_dialog_close (ADW_DIALOG (user_data));
}

void
nuvashalauncher_ui_show_toast (GtkWidget  *page,
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

void
nuvashalauncher_ui_set_action_label (GtkWidget *button, const char *label)
{
  g_return_if_fail (GTK_IS_WIDGET (button));
  g_return_if_fail (label != NULL);

  gtk_widget_set_tooltip_text (button, label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

void
nuvashalauncher_ui_prepare_icon_button (GtkWidget *button, const char *label)
{
  g_return_if_fail (GTK_IS_WIDGET (button));
  g_return_if_fail (label != NULL);

  gtk_widget_add_css_class (button, "flat");
  nuvashalauncher_ui_set_action_label (button, label);
}

void
nuvashalauncher_ui_prepare_dialog_content (GtkWidget *content)
{
  g_return_if_fail (GTK_IS_WIDGET (content));
  gtk_widget_add_css_class (content, "nuvashalauncher-dialog-content");
  gtk_widget_set_hexpand (content, TRUE);
  gtk_widget_set_vexpand (content, TRUE);
}

GtkCheckButton *
nuvashalauncher_ui_selection_prefix_new (GtkWidget *icon, GtkStack **out_stack)
{
  g_return_val_if_fail (GTK_IS_WIDGET (icon), NULL);

  GtkCheckButton *check = GTK_CHECK_BUTTON (gtk_check_button_new ());
  GtkStack *icon_stack = GTK_STACK (gtk_stack_new ());
  GtkBox *badge_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkImage *check_icon = GTK_IMAGE (gtk_image_new_from_icon_name ("object-select-symbolic"));

  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);

  gtk_widget_add_css_class (GTK_WIDGET (badge_box), "nuvashalauncher-content-icon");
  gtk_widget_add_css_class (GTK_WIDGET (badge_box), "nuvashalauncher-select-badge");
  gtk_widget_set_size_request (GTK_WIDGET (badge_box),
                               NUVASHALAUNCHER_ICON_SIZE, NUVASHALAUNCHER_ICON_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (badge_box), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (badge_box), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (badge_box), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (badge_box), TRUE);

  gtk_image_set_pixel_size (check_icon, 24);
  gtk_widget_set_valign (GTK_WIDGET (check_icon), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (check_icon), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (check_icon), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (check_icon), TRUE);
  gtk_box_append (badge_box, GTK_WIDGET (check_icon));

  gtk_stack_add_named (icon_stack, icon, "icon");
  gtk_stack_add_named (icon_stack, GTK_WIDGET (badge_box), "check");
  gtk_stack_set_visible_child_name (icon_stack, "icon");
  gtk_widget_set_size_request (GTK_WIDGET (icon_stack),
                               NUVASHALAUNCHER_ICON_SIZE, NUVASHALAUNCHER_ICON_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (icon_stack), GTK_ALIGN_CENTER);

  gtk_check_button_set_child (check, GTK_WIDGET (icon_stack));
  gtk_widget_add_css_class (GTK_WIDGET (check), "nuvashalauncher-select-checkbox");
  gtk_widget_set_valign (GTK_WIDGET (check), GTK_ALIGN_CENTER);

  if (out_stack != NULL)
    *out_stack = icon_stack;
  return check;
}

GtkActionBar *
nuvashalauncher_ui_selection_footer_new (GtkLabel **out_label,
                                         GtkButton **out_delete_button)
{
  GtkActionBar *footer = GTK_ACTION_BAR (gtk_action_bar_new ());
  GtkLabel *label = GTK_LABEL (gtk_label_new (""));
  GtkButton *del = GTK_BUTTON (gtk_button_new_from_icon_name ("edit-delete-symbolic"));

  gtk_action_bar_set_center_widget (footer, GTK_WIDGET (label));
  gtk_action_bar_pack_end (footer, GTK_WIDGET (del));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (del), _("Delete"));
  gtk_widget_set_visible (GTK_WIDGET (del), FALSE);
  gtk_action_bar_set_revealed (footer, FALSE);

  if (out_label != NULL)
    *out_label = label;
  if (out_delete_button != NULL)
    *out_delete_button = del;
  return footer;
}

void
nuvashalauncher_ui_selection_footer_update (GtkActionBar *footer,
                                            GtkLabel *label,
                                            GtkButton *delete_button,
                                            guint selected,
                                            guint total)
{
  g_autofree char *text = g_strdup_printf (_("%u/%u selected"), selected, total);

  if (label != NULL)
    gtk_label_set_text (label, text);
  if (footer != NULL)
    gtk_action_bar_set_revealed (footer, selected > 0);
  if (delete_button != NULL)
    {
      gtk_widget_set_visible (GTK_WIDGET (delete_button), selected > 0);
      gtk_widget_set_sensitive (GTK_WIDGET (delete_button), selected > 0);
    }
}
