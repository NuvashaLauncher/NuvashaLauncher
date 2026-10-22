/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-main-window-private.h"
#include "nuvasha-cache.h"
#include "nuvasha-create-instance-dialog.h"
#include "nuvasha-download-modpack-dialog.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-instance-page.h"
#include "nuvasha-i18n.h"
#include "nuvasha-settings.h"
#include "nuvasha-users-popover.h"

static void
on_main_window_banner_dismiss (AdwBanner *banner, gpointer user_data G_GNUC_UNUSED)
{
  adw_banner_set_revealed (banner, FALSE);
}

void
nuvasha_main_window_build_ui (NuvashaMainWindow *self)
{

  g_autoptr (GMenu) add_menu = create_add_menu ();
  g_autoptr (GMenu) app_menu = create_app_menu ();
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  GtkMenuButton *add_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  GtkMenuButton *users_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  GtkMenuButton *app_menu_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  GtkWidget *users_child = NULL;
  AdwStatusPage *empty_page = ADW_STATUS_PAGE (adw_status_page_new ());
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  GtkWidget *add_popover = NULL;

  /* Use a raised header so it has its own surface. */
  gtk_widget_add_css_class (GTK_WIDGET (toolbar), "nuvasha-main-toolbar");
  adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (toolbar),
                                      ADW_TOOLBAR_RAISED);

  g_action_map_add_action_entries (G_ACTION_MAP (self), window_entries,
                                   window_entries_count, self);

  /* Start of the header: the "+" menu. */
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (add_button),
                                 "list-add-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (add_button), _("Add"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (add_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, _("Add"), -1);
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (add_button),
                                  G_MENU_MODEL (add_menu));
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (add_button));

  add_popover = GTK_WIDGET (
    gtk_menu_button_get_popover (GTK_MENU_BUTTON (add_button)));
  if (add_popover != NULL)
     set_menu_item_tooltip (add_popover, _("Download Modpack"),
                            _("Search Modrinth and install a modpack"));

  /* Show active downloads and the content-page selection count. Hide the
   * button when both are empty. */
  self->downloads.icon = GTK_IMAGE (gtk_image_new_from_icon_name (
    "folder-download-symbolic"));
   self->downloads.spinner = ADW_SPINNER (adw_spinner_new ());
  self->downloads.button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  self->downloads.list = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  self->downloads.rows = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               NULL, download_row_free);

  {
    GtkBox *packed = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
    GtkBox *icon_row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4));
    GtkPopover *popover = GTK_POPOVER (gtk_popover_new ());
    GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());

    /* Place the download icon and spinner in the button. */
    gtk_widget_set_valign (GTK_WIDGET (self->downloads.icon), GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (icon_row), GTK_WIDGET (self->downloads.icon));
    gtk_widget_set_valign (GTK_WIDGET (self->downloads.spinner), GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (icon_row), GTK_WIDGET (self->downloads.spinner));

    gtk_menu_button_set_child (GTK_MENU_BUTTON (self->downloads.button),
                               GTK_WIDGET (icon_row));
    gtk_widget_add_css_class (GTK_WIDGET (self->downloads.button), "flat");
    gtk_menu_button_set_always_show_arrow (
      GTK_MENU_BUTTON (self->downloads.button), FALSE);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->downloads.button), _("Downloads"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (self->downloads.button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, _("Downloads"),
                                    -1);
    gtk_widget_set_visible (GTK_WIDGET (self->downloads.button), FALSE);

    gtk_box_append (GTK_BOX (packed), GTK_WIDGET (self->downloads.button));

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (scroller), TRUE);
    gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (scroller), 360);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                   GTK_WIDGET (self->downloads.list));
    gtk_popover_set_child (GTK_POPOVER (popover), GTK_WIDGET (scroller));
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->downloads.button),
                                 GTK_WIDGET (popover));

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (packed));
  }

  g_signal_connect_object (nuvasha_downloads_get_default (), "changed",
                           G_CALLBACK (on_downloads_changed), self, 0);

  /* Add the account selector and application menu to the header. */
  /* The model owns the popover and its account store. */
  users_child = nuvasha_users_popover_get_button_content (self->model->users_popover);
  gtk_menu_button_set_child (GTK_MENU_BUTTON (users_button), users_child);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (users_button),
                              GTK_WIDGET (self->model->users_popover));
  /* The child is a box around the avatar, so the menu button does not
     recognize it as an icon button and would draw a frame behind it. */
  gtk_widget_add_css_class (GTK_WIDGET (users_button), "flat");
  /* Provide an accessible label for the icon-only button. */
  gtk_accessible_update_property (GTK_ACCESSIBLE (users_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, _("Accounts"),
                                  -1);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (app_menu_button),
                                 "open-menu-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (app_menu_button), _("Application menu"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (app_menu_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Application menu"), -1);
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app_menu_button),
                                  G_MENU_MODEL (app_menu));

  adw_header_bar_pack_end (ADW_HEADER_BAR (header), GTK_WIDGET (app_menu_button));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), GTK_WIDGET (users_button));

  /* The empty state has no button in the middle: the "+" menu is the
   * only way to create an instance. */
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), _("No Instances Yet"));
  adw_status_page_set_description (ADW_STATUS_PAGE (empty_page),
                                   _("Use the + button in the header to create one"));
  gtk_widget_set_hexpand (GTK_WIDGET (empty_page), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (empty_page), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (empty_page), "nuvasha-empty-state");

  self->ui.stack = ADW_VIEW_STACK (adw_view_stack_new ());
  adw_view_stack_add_named (self->ui.stack, GTK_WIDGET (empty_page), "empty");
  gtk_widget_set_hexpand (GTK_WIDGET (self->ui.stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->ui.stack), TRUE);

  /* The list is the content of the window: an instance is a window of
     its own and is never a page here. */
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar),
                                GTK_WIDGET (self->ui.stack));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (header));

  /* Keep launch failures in a banner so the user can read them later. */
  self->ui.banner = ADW_BANNER (adw_banner_new (""));
  adw_banner_set_button_label (ADW_BANNER (self->ui.banner), _("Dismiss"));
  adw_banner_set_revealed (ADW_BANNER (self->ui.banner), FALSE);
  g_signal_connect_object (self->ui.banner, "button-clicked",
                           G_CALLBACK (on_main_window_banner_dismiss), self, 0);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar),
                                GTK_WIDGET (self->ui.banner));

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      GTK_WIDGET (toolbar));

  gtk_window_set_title (GTK_WINDOW (self), _("Nuvasha Launcher"));
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 640);

  /* A download may already be running (a window opened while one does),
     so the header starts agreeing with the manager rather than assuming
     nothing is on its way. */
  refresh_downloads (self);

}
