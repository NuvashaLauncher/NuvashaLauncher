/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-main-window-private.h"
#include "aml-cache.h"
#include "aml-create-instance-dialog.h"
#include "aml-download-modpack-dialog.h"
#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-instance-page.h"
#include "aml-i18n.h"
#include "aml-settings.h"
#include "aml-users-popover.h"

static void
on_main_window_banner_dismiss (AdwBanner *banner, gpointer user_data G_GNUC_UNUSED)
{
  adw_banner_set_revealed (banner, FALSE);
}

void
aml_main_window_build_ui (AmlMainWindow *self)
{

  g_autoptr (GMenu) add_menu = create_add_menu ();
  g_autoptr (GMenu) app_menu = create_app_menu ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *add_button = gtk_menu_button_new ();
  GtkWidget *users_button = gtk_menu_button_new ();
  GtkWidget *app_menu_button = gtk_menu_button_new ();
  GtkWidget *users_child = NULL;
  GtkWidget *empty_page = adw_status_page_new ();
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *add_popover = NULL;

  /* Flat is the default top-bar style, and it paints no surface of its
     own, so the header would take the window background and read as the
     same colour as the content under it. Raised is what gives a header
     the colour every Adwaita header has. */
  adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (toolbar),
                                      ADW_TOOLBAR_RAISED);

  /* The settings and the accounts are the window's own: every launcher of
     an instance is handed the same ones, so the whole window agrees on
     what a launch does and as whom it plays. */
  self->services.settings = aml_settings_new ();
  aml_download_set_cache_settings (self->services.settings);
  self->services.accounts = aml_accounts_new ();

  g_action_map_add_action_entries (G_ACTION_MAP (self), window_entries,
                                   window_entries_count, self);

  /* Start of the header: the "+" menu. */
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (add_button),
                                 "list-add-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (add_button),
                                  G_MENU_MODEL (add_menu));
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), add_button);

  add_popover = GTK_WIDGET (
    gtk_menu_button_get_popover (GTK_MENU_BUTTON (add_button)));
  if (add_popover != NULL)
    set_menu_item_tooltip (add_popover, "Download Modpack",
                           "Search Modrinth and install a modpack");

  /* Beside the "+": the downloads. A spinner appears while anything is
     downloading and opens a dropdown of what is; the icon beside it
     carries the number of what the content pages have picked and
     installed at once. The button is hidden when there is neither. */
  self->downloads.icon = gtk_image_new_from_icon_name (
    "folder-download-symbolic");
   self->downloads.spinner = adw_spinner_new ();
  self->downloads.button = gtk_menu_button_new ();
  self->downloads.list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->downloads.rows = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               NULL, download_row_free);

  {
    GtkWidget *packed = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *icon_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *popover = gtk_popover_new ();
    GtkWidget *scroller = gtk_scrolled_window_new ();

    /* The icon and the spinner share the middle: the icon says what the
       button opens, the spinner that something is on its way. */
    gtk_widget_set_valign (self->downloads.icon, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (icon_row), self->downloads.icon);
    gtk_widget_set_valign (self->downloads.spinner, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (icon_row), self->downloads.spinner);

    gtk_menu_button_set_child (GTK_MENU_BUTTON (self->downloads.button),
                               icon_row);
    gtk_widget_add_css_class (self->downloads.button, "flat");
    gtk_menu_button_set_always_show_arrow (
      GTK_MENU_BUTTON (self->downloads.button), FALSE);
  gtk_widget_set_tooltip_text (self->downloads.button, _("Downloads"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (self->downloads.button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, _("Downloads"),
                                    -1);
    gtk_widget_set_visible (self->downloads.button, FALSE);

    gtk_box_append (GTK_BOX (packed), self->downloads.button);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (scroller), TRUE);
    gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (scroller), 360);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                   self->downloads.list);
    gtk_popover_set_child (GTK_POPOVER (popover), scroller);
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->downloads.button),
                                 popover);

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), packed);
  }

  g_signal_connect_object (aml_downloads_get_default (), "changed",
                           G_CALLBACK (on_downloads_changed), self, 0);

  /* End of the header: the accounts, then the application menu. The
   * button shows the avatar of the active account, and what is behind it
   * is the list of the accounts this copy of AML has, which is also where
   * a new one is added. */
  self->services.users_popover = aml_users_popover_new (self->services.accounts);
  /* A widget is created with a floating reference, and set_popover()
     below sinks that one for the button. The window keeps a reference of
     its own as well, so that dropping it in dispose() never frees a
     popover the button still holds. */
  g_object_ref_sink (self->services.users_popover);
  users_child = aml_users_popover_get_button_content (self->services.users_popover);
  gtk_menu_button_set_child (GTK_MENU_BUTTON (users_button), users_child);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (users_button),
                              GTK_WIDGET (self->services.users_popover));
  /* The child is a box around the avatar, so the menu button does not
     recognize it as an icon button and would draw a frame behind it. */
  gtk_widget_add_css_class (users_button, "flat");
  /* The button is an icon and nothing else, so it needs to be named for
     what it opens rather than read from a label that is not there. */
  gtk_accessible_update_property (GTK_ACCESSIBLE (users_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, _("Accounts"),
                                  -1);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (app_menu_button),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app_menu_button),
                                  G_MENU_MODEL (app_menu));

  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app_menu_button);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), users_button);

  /* The empty state has no button in the middle: the "+" menu is the
   * only way to create an instance. */
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), _("No Instances Yet"));
  adw_status_page_set_description (ADW_STATUS_PAGE (empty_page),
                                   _("Use the + button in the header to create one"));
  gtk_widget_set_hexpand (empty_page, TRUE);
  gtk_widget_set_vexpand (empty_page, TRUE);

  self->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->stack), empty_page, "empty");
  gtk_widget_set_hexpand (self->stack, TRUE);
  gtk_widget_set_vexpand (self->stack, TRUE);

  /* The list is the content of the window: an instance is a window of
     its own and is never a page here. */
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), self->stack);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  /* A launch that ends badly is said in a banner under the header, where
     it stays until it is read: a toast is gone in seconds, which is
     exactly when a reason is worth having. */
  self->banner = adw_banner_new ("");
  adw_banner_set_button_label (ADW_BANNER (self->banner), "Dismiss");
  adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
  g_signal_connect (self->banner, "button-clicked",
                    G_CALLBACK (on_main_window_banner_dismiss), NULL);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), self->banner);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      GTK_WIDGET (toolbar));

  gtk_window_set_title (GTK_WINDOW (self), "AML");
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 640);

  /* A download may already be running (a window opened while one does),
     so the header starts agreeing with the manager rather than assuming
     nothing is on its way. */
  refresh_downloads (self);

}
