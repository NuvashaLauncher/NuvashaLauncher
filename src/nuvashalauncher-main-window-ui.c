/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-create-instance-dialog.h"
#include "nuvashalauncher-download-modpack-dialog.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-instance-page.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-users-popover.h"
#include "nuvashalauncher-ui-feedback.h"

static void
on_main_window_banner_dismiss (AdwBanner *banner, gpointer user_data G_GNUC_UNUSED)
{
  adw_banner_set_revealed (banner, FALSE);
}

static void
on_main_window_width_changed (GObject *object,
                               GParamSpec *pspec G_GNUC_UNUSED,
                               gpointer user_data G_GNUC_UNUSED)
{
  nuvashalauncherMainWindow *self = nuvashalauncher_MAIN_WINDOW (object);
  int width = gtk_widget_get_width (GTK_WIDGET (self));

  if (self->ui.toolbar == NULL || width <= 0)
    return;
  adw_toolbar_view_set_top_bar_style (
    self->ui.toolbar,
    width < 700 ? ADW_TOOLBAR_FLAT : ADW_TOOLBAR_RAISED);
}

void
nuvashalauncher_main_window_build_ui (nuvashalauncherMainWindow *self)
{

  g_autoptr (GMenu) add_menu = create_add_menu ();
  g_autoptr (GMenu) app_menu = create_app_menu ();
  g_autoptr (GtkBuilder) shell_builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-main-shell.ui");
  AdwHeaderBar *header = ADW_HEADER_BAR (gtk_builder_get_object (
    shell_builder, "header"));
  GtkMenuButton *add_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  GtkMenuButton *users_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  GtkMenuButton *app_menu_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  GtkButton *edit_button = GTK_BUTTON (gtk_button_new_from_icon_name (
    "document-edit-symbolic"));
  GtkButton *footer_delete = GTK_BUTTON (gtk_button_new_from_icon_name (
    "user-trash-symbolic"));
  GtkButton *footer_export = GTK_BUTTON (gtk_button_new_from_icon_name (
    "document-save-symbolic"));
  GtkActionBar *selection_footer = GTK_ACTION_BAR (gtk_action_bar_new ());
  GtkLabel *selection_label = GTK_LABEL (gtk_label_new (""));
  GtkWidget *users_child = NULL;
  AdwStatusPage *empty_page = ADW_STATUS_PAGE (adw_status_page_new ());
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (gtk_builder_get_object (
    shell_builder, "toolbar"));
  GtkWidget *add_popover = NULL;

  self->ui.toolbar = toolbar;
  g_signal_connect_object (self, "notify::width",
                           G_CALLBACK (on_main_window_width_changed), self, 0);

  /* Use a raised header so it has its own surface. */
  gtk_widget_add_css_class (GTK_WIDGET (toolbar), "nuvashalauncher-main-toolbar");
  adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (toolbar),
                                      ADW_TOOLBAR_RAISED);

  g_action_map_add_action_entries (G_ACTION_MAP (self), window_entries,
                                   window_entries_count, self);

  /* Start of the header: the "+" menu. */
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (add_button),
                                 "list-add-symbolic");
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (add_button), _("Add"));
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (add_button),
                                  G_MENU_MODEL (add_menu));
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (add_button));

  self->ui.edit = edit_button;
  self->ui.footer_delete = footer_delete;
  self->ui.footer_export = footer_export;
  self->ui.selection_footer = selection_footer;
  self->ui.selection_label = selection_label;
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (edit_button), _("Edit mode"));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (footer_delete), _("Delete"));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (footer_export), _("Export"));
  gtk_widget_set_visible (GTK_WIDGET (footer_delete), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (footer_export), FALSE);
  gtk_action_bar_set_center_widget (selection_footer, GTK_WIDGET (selection_label));
  gtk_action_bar_pack_end (selection_footer, GTK_WIDGET (footer_export));
  gtk_action_bar_pack_end (selection_footer, GTK_WIDGET (footer_delete));
  gtk_action_bar_set_revealed (selection_footer, FALSE);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (edit_button));
  adw_toolbar_view_add_bottom_bar (toolbar, GTK_WIDGET (selection_footer));
  adw_toolbar_view_set_bottom_bar_style (toolbar, ADW_TOOLBAR_RAISED);
  adw_toolbar_view_set_reveal_bottom_bars (toolbar, TRUE);
  g_signal_connect_swapped (edit_button, "clicked",
                            G_CALLBACK (set_instance_edit_mode), self);
  g_signal_connect_swapped (footer_delete, "clicked",
                            G_CALLBACK (bulk_delete_instances), self);
  g_signal_connect_swapped (footer_export, "clicked",
                            G_CALLBACK (bulk_export_instances), self);

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
    gtk_menu_button_set_always_show_arrow (
      GTK_MENU_BUTTON (self->downloads.button), FALSE);
    nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (self->downloads.button),
                                    _("Downloads"));
    gtk_widget_set_visible (GTK_WIDGET (self->downloads.button), FALSE);

    gtk_box_append (GTK_BOX (packed), GTK_WIDGET (self->downloads.button));

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (scroller), TRUE);
    gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (scroller), 360);
    gtk_widget_set_size_request (GTK_WIDGET (scroller), 360, -1);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                   GTK_WIDGET (self->downloads.list));
    gtk_popover_set_child (GTK_POPOVER (popover), GTK_WIDGET (scroller));
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->downloads.button),
                                 GTK_WIDGET (popover));

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (packed));
  }

  g_signal_connect_object (nuvashalauncher_downloads_get_default (), "changed",
                           G_CALLBACK (on_downloads_changed), self, 0);

  /* Add the account selector and application menu to the header. */
  /* The model owns the popover and its account store. */
  users_child = nuvashalauncher_users_popover_get_button_content (self->model->users_popover);
  gtk_menu_button_set_child (GTK_MENU_BUTTON (users_button), users_child);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (users_button),
                              GTK_WIDGET (self->model->users_popover));
  /* The child is a box around the avatar, so the menu button does not
     recognize it as an icon button and would draw a frame behind it. */
   nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (users_button), _("Accounts"));

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (app_menu_button),
                                 "open-menu-symbolic");
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (app_menu_button),
                                  _("Application menu"));
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
  gtk_widget_add_css_class (GTK_WIDGET (empty_page), "nuvashalauncher-empty-state");

  self->ui.stack = ADW_VIEW_STACK (gtk_builder_get_object (shell_builder, "stack"));
  adw_view_stack_add_named (self->ui.stack, GTK_WIDGET (empty_page), "empty");
  gtk_widget_set_hexpand (GTK_WIDGET (self->ui.stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->ui.stack), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->ui.stack), "nuvashalauncher-main-content");

  /* The list is the content of the window: an instance is a window of
     its own and is never a page here. */
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

    gtk_window_set_title (GTK_WINDOW (self), "Nuvasha Launcher");
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 640);

  /* A download may already be running (a window opened while one does),
     so the header starts agreeing with the manager rather than assuming
     nothing is on its way. */
  refresh_downloads (self);

}
