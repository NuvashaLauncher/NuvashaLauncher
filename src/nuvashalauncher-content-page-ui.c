/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-filter-entry.h"



/* Create a flat toolbar button with a symbolic icon. The label is used as
 * both the tooltip and the accessible name. */
static GtkWidget *
clamped_group (AdwPreferencesGroup *group)
{
  AdwClamp *clamp = ADW_CLAMP (adw_clamp_new ());

  gtk_widget_set_margin_top (GTK_WIDGET (clamp), 8);
  gtk_widget_set_margin_bottom (GTK_WIDGET (clamp), 16);
  gtk_widget_set_margin_start (GTK_WIDGET (clamp), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (clamp), 12);
  adw_clamp_set_child (clamp, GTK_WIDGET (group));
  return GTK_WIDGET (clamp);
}

static GtkWidget *
toolbar_button (const char *icon_name, const char *label, GCallback callback,
                gpointer user_data)
{
  GtkButton *button = GTK_BUTTON (gtk_button_new_from_icon_name (icon_name));

  gtk_widget_set_valign (GTK_WIDGET (button), GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (button), label);
  g_signal_connect_object (button, "clicked", callback, user_data, 0);

  return GTK_WIDGET (button);
}

void
nuvashalauncher_content_page_set_installed_search_visible (
  NuvashaLauncherContentPage *self, gboolean visible)
{
  if (nuvashalauncher_content_page_is_browsing (self))
    {
      if (self->browse.search_bar == NULL ||
          !GTK_IS_WIDGET (self->browse.search_bar))
        return;

      gtk_widget_set_visible (GTK_WIDGET (self->browse.search_bar), visible);
      gtk_widget_set_hexpand (GTK_WIDGET (self->browse.search_bar), visible);
      if (self->header_bar != NULL && self->header_search_clamp != NULL)
        gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp), visible);
      if (visible && self->browse.search_entry != NULL)
        gtk_widget_grab_focus (GTK_WIDGET (self->browse.search_entry));
      else if (!visible)
        {
          if (self->browse.search_entry != NULL)
            gtk_editable_set_text (GTK_EDITABLE (self->browse.search_entry), "");
          gtk_widget_grab_focus (GTK_WIDGET (self));
        }
      return;
    }

  if (self->installed.search_entry == NULL ||
      !GTK_IS_WIDGET (self->installed.search_entry))
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->installed.search_entry), visible);
  gtk_widget_set_hexpand (GTK_WIDGET (self->installed.search_entry), visible);
  if (self->header_bar != NULL && self->header_search_clamp != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp), visible);
  if (self->header_bar == NULL && self->installed.search_spacer != NULL)
    gtk_widget_set_hexpand (GTK_WIDGET (self->installed.search_spacer), !visible);
  if (visible)
    gtk_widget_grab_focus (GTK_WIDGET (self->installed.search_entry));
  else
    {
      gtk_editable_set_text (GTK_EDITABLE (self->installed.search_entry), "");
      gtk_widget_grab_focus (GTK_WIDGET (self));
    }
}

static void
on_installed_empty_browse_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherContentPage *self = user_data;
  if (self->switcher_browse != NULL)
    gtk_toggle_button_set_active (self->switcher_browse, TRUE);
}

static void
on_installed_search_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherContentPage *self = user_data;

  nuvashalauncher_content_page_set_installed_search_visible (
    self, !gtk_widget_get_visible (GTK_WIDGET (self->installed.search_entry)));
}


/* ---------------------------------------------------------- *
 * Building the page
 * ---------------------------------------------------------- */

void
nuvashalauncher_content_page_build_installed (NuvashaLauncherContentPage *self)
{
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
   GtkBox *actions = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
   GtkSearchEntry *search_bar = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
   GtkButton *search_button = GTK_BUTTON (gtk_button_new_from_icon_name ("system-search-symbolic"));
   GtkBox *actions_spacer = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  GtkBox *content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  AdwSpinner *loading = ADW_SPINNER (adw_spinner_new ());
  GtkStack *update_content = GTK_STACK (gtk_stack_new ());
  GtkImage *update_icon = GTK_IMAGE (gtk_image_new_from_icon_name (
    "software-update-available-symbolic"));
  AdwSpinner *update_spinner = ADW_SPINNER (adw_spinner_new ());
   AdwPreferencesGroup *group =
     ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
   GtkButton *update_all = GTK_BUTTON (gtk_button_new ());
    GtkPopover *update_popover = GTK_POPOVER (gtk_popover_new ());
    GtkScrolledWindow *update_scroller = GTK_SCROLLED_WINDOW (
      gtk_scrolled_window_new ());
    g_autoptr (GtkBuilder) update_builder = gtk_builder_new_from_resource (
      "/com/github/NuvashaLauncher/NuvashaLauncher/ui/nuvashalauncher-download-row.ui");
    GtkBox *update_box = GTK_BOX (gtk_builder_get_object (update_builder, "box"));
    GtkLabel *update_title = GTK_LABEL (gtk_builder_get_object (update_builder, "title"));
    GtkLabel *update_phase = GTK_LABEL (gtk_builder_get_object (update_builder, "phase"));
    GtkLabel *update_count = GTK_LABEL (gtk_builder_get_object (update_builder, "count"));
    GtkProgressBar *update_bar = GTK_PROGRESS_BAR (gtk_builder_get_object (update_builder, "bar"));
    GtkButton *update_cancel = GTK_BUTTON (gtk_builder_get_object (update_builder, "cancel"));

   gtk_widget_set_hexpand (GTK_WIDGET (actions), TRUE);
   self->installed.search_bar = GTK_SEARCH_ENTRY (g_object_ref (search_bar));
    self->installed.search_entry = GTK_SEARCH_ENTRY (search_bar);
     self->installed.search_button = GTK_BUTTON (g_object_ref (search_button));
    self->installed.search_spacer = actions_spacer;
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_bar),
                                            _("Search"));
      gtk_widget_set_visible (GTK_WIDGET (search_bar), FALSE);
    g_signal_connect_object (search_bar, "changed",
                             G_CALLBACK (nuvashalauncher_content_page_on_installed_search_changed), self, 0);
      nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (search_button), _("Search"));
      gtk_widget_set_valign (GTK_WIDGET (search_button), GTK_ALIGN_CENTER);
      g_signal_connect (search_button, "clicked",
                        G_CALLBACK (on_installed_search_clicked), self);
      gtk_box_append (GTK_BOX (actions), GTK_WIDGET (search_button));
      gtk_box_append (GTK_BOX (actions), GTK_WIDGET (search_bar));
      gtk_widget_set_hexpand (GTK_WIDGET (actions_spacer), TRUE);
      gtk_box_append (GTK_BOX (actions), GTK_WIDGET (actions_spacer));

     gtk_button_set_child (update_all, GTK_WIDGET (update_content));
     gtk_widget_set_valign (GTK_WIDGET (update_all), GTK_ALIGN_CENTER);
     nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (update_all), _("Update all"));
    g_signal_connect (update_all, "clicked",
                      G_CALLBACK (nuvashalauncher_content_page_on_update_all_clicked), self);
   gtk_stack_add_named (update_content, GTK_WIDGET (update_icon), "icon");
   gtk_stack_add_named (update_content, GTK_WIDGET (update_spinner), "spinner");
   gtk_stack_set_visible_child_name (update_content, "icon");
      gtk_widget_set_parent (GTK_WIDGET (update_popover), GTK_WIDGET (actions));
      gtk_widget_set_visible (GTK_WIDGET (update_popover), FALSE);
    gtk_label_set_text (update_title, _("Fetching updates"));
     gtk_label_set_text (update_phase, _("Progress"));
    nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (update_cancel), _("Cancel"));
    gtk_widget_set_visible (GTK_WIDGET (update_cancel), FALSE);
    g_signal_connect (update_cancel, "clicked",
                      G_CALLBACK (nuvashalauncher_content_page_on_update_cancel), self);
    gtk_scrolled_window_set_policy (update_scroller, GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (update_scroller, TRUE);
    gtk_scrolled_window_set_max_content_height (update_scroller, 360);
     gtk_widget_set_size_request (GTK_WIDGET (update_scroller), 320, -1);
    gtk_scrolled_window_set_child (update_scroller, GTK_WIDGET (update_box));
    gtk_popover_set_child (update_popover, GTK_WIDGET (update_scroller));
    gtk_popover_set_has_arrow (update_popover, TRUE);
   self->installed.update_content = update_content;
   self->installed.update_spinner = update_spinner;
   self->installed.update_popover = update_popover;
   g_object_add_weak_pointer (G_OBJECT (update_popover),
                              (gpointer *) &self->installed.update_popover);
    self->installed.update_phase = update_phase;
   self->installed.update_count = update_count;
   self->installed.update_bar = update_bar;
    self->installed.update_cancel = update_cancel;
    self->installed.update_all = update_all;
   gtk_widget_set_sensitive (GTK_WIDGET (self->installed.update_all), FALSE);
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->installed.update_all));
  gtk_box_append (GTK_BOX (actions),
                  toolbar_button ("folder-symbolic", _("Open folder"),
                                  G_CALLBACK (nuvashalauncher_content_page_on_open_folder_clicked), self));

  /* The actions belong to the toolbar row above both lists, not to this
     one of them: they are the same row whichever list is shown, and
     moving them keeps a download in progress from being thrown away by a
     visit to the other list.

     The row of the other list is taken out of the bar when the lists are
     switched, so this one is held here: without it, taking it out would
     be the last reference to it and would destroy the box. */
   self->installed.actions = g_object_ref_sink (GTK_BOX (actions));
  g_object_ref_sink (actions);

   self->installed.status = ADW_STATUS_PAGE (adw_status_page_new ());
   gtk_widget_set_vexpand (GTK_WIDGET (self->installed.status), TRUE);
   self->installed.loading = loading;
   gtk_widget_set_size_request (GTK_WIDGET (loading), 64, 64);
   gtk_widget_set_valign (GTK_WIDGET (loading), GTK_ALIGN_CENTER);
   gtk_widget_set_halign (GTK_WIDGET (loading), GTK_ALIGN_CENTER);
   gtk_widget_set_vexpand (GTK_WIDGET (loading), TRUE);
   gtk_widget_set_visible (GTK_WIDGET (loading), FALSE);
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->installed.status),
                                 nuvashalauncher_content_type_icon (self->type));
  adw_status_page_set_title (ADW_STATUS_PAGE (self->installed.status),
                               _("Nothing installed yet"));

  GtkWidget *empty_browse_btn = gtk_button_new_with_label (_("Browse"));
  gtk_widget_add_css_class (empty_browse_btn, "suggested-action");
  gtk_widget_add_css_class (empty_browse_btn, "pill");
  gtk_widget_set_halign (empty_browse_btn, GTK_ALIGN_CENTER);
  g_signal_connect_object (empty_browse_btn, "clicked",
                           G_CALLBACK (on_installed_empty_browse_clicked), self, 0);
  adw_status_page_set_child (ADW_STATUS_PAGE (self->installed.status), empty_browse_btn);

  /* The list starts where the row above it ends: its first row is not
     rounded at the top, so the two read as one piece. */

   self->installed.group = group;
   self->installed.scroller = GTK_SCROLLED_WINDOW (scroller);
   {
     GtkAdjustment *vadj =
       gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));
     g_signal_connect_object (
       vadj, "value-changed",
       G_CALLBACK (nuvashalauncher_content_page_on_installed_scroll_changed),
       self, 0);
     g_signal_connect_object (
       vadj, "changed",
       G_CALLBACK (nuvashalauncher_content_page_on_installed_scroll_changed),
       self, 0);
   }
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 clamped_group (group));
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_widget_set_vexpand (GTK_WIDGET (content), TRUE);
   gtk_box_append (GTK_BOX (content), GTK_WIDGET (loading));
   gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->installed.status));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (scroller));
  gtk_widget_set_visible (GTK_WIDGET (scroller), FALSE);
   gtk_box_append (GTK_BOX (box), GTK_WIDGET (content));

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack), GTK_WIDGET (box),
                                        "installed", _("Installed"),
                                      "view-list-symbolic");
}

void
nuvashalauncher_content_page_on_stack_switched (GObject *stack, GParamSpec *pspec G_GNUC_UNUSED,
                   gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  const char *visible = adw_view_stack_get_visible_child_name (
      ADW_VIEW_STACK (stack));
  gboolean browsing = g_strcmp0 (visible, "browse") == 0;

  /* The switcher follows the list, however the list was changed: a page
     that is built again keeps the button of the page on top on. */
  if (self->switcher_installed != NULL && self->switcher_browse != NULL)
    gtk_toggle_button_set_active (
      GTK_TOGGLE_BUTTON (browsing ? self->switcher_browse
                                  : self->switcher_installed), TRUE);

  /* The row shows the actions of whichever list is on top. */
  nuvashalauncher_content_page_move_actions_into_bar (self, browsing);

  if (!browsing || self->browse.model->searched)
    return;

  self->browse.model->searched = TRUE;
  nuvashalauncher_content_page_run_search (self, FALSE);
}

void
nuvashalauncher_content_page_reset_for_tab_switch (NuvashaLauncherContentPage *self)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_CONTENT_PAGE (self));

   if (self->browse.filter_popover != NULL)
     {
       gtk_popover_popdown (self->browse.filter_popover);
     }

  if (self->stack != NULL)
    adw_view_stack_set_visible_child_name (self->stack, "installed");

  if (!self->installed.loaded)
    reload_installed (self);
}

void
nuvashalauncher_content_page_build_browse (NuvashaLauncherContentPage *self)
{
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkBox *top = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkEntry *search_bar = GTK_ENTRY (g_object_new (NUVASHALAUNCHER_TYPE_FILTER_ENTRY, NULL));
  GtkPopover *popover = GTK_POPOVER (gtk_popover_new ());
  GtkBox *filter_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkScrolledWindow *filter_scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  GtkBox *content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  GtkBox *loading_more = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  AdwSpinner *loading_spinner = ADW_SPINNER (adw_spinner_new ());
  GtkLabel *loading_label = GTK_LABEL (gtk_label_new (_("Loading more…")));
  GtkBox *retry_row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkLabel *retry_label = GTK_LABEL (gtk_label_new (NULL));
  GtkButton *retry_button = GTK_BUTTON (gtk_button_new_with_label (_("Retry")));
  AdwPreferencesGroup *group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

   self->browse.search_entry = search_bar;
   self->browse.search_bar = GTK_ENTRY (g_object_ref (search_bar));
  /* The field takes the room the row has, the way the modpack browser's
     search does, rather than a fixed width of its own. */
  gtk_widget_set_hexpand (GTK_WIDGET (search_bar), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (search_bar), FALSE);
  gtk_entry_set_placeholder_text (search_bar,
                                            _("Search"));

  g_signal_connect_object (search_bar, "changed",
                           G_CALLBACK (nuvashalauncher_content_page_on_search_changed), self, 0);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_BUBBLE);
    gtk_widget_add_controller (GTK_WIDGET (search_bar), keys);
    g_signal_connect_object (keys, "key-pressed",
                             G_CALLBACK (nuvashalauncher_content_page_on_search_key), self, 0);
  }

   nuvashalauncher_filter_entry_set_popover (
     NUVASHALAUNCHER_FILTER_ENTRY (search_bar), popover, _("Filter by category"));
   gtk_box_append (GTK_BOX (top), GTK_WIDGET (search_bar));
  /* Load categories when the leading filter icon opens the popover. */
   g_signal_connect_object (popover, "show",
                            G_CALLBACK (nuvashalauncher_content_page_on_filter_clicked), self, 0);

   /* The row takes the room the bar has; the filter replaces the search
      icon at the start, leaving the clear button at the other end. */
  gtk_widget_set_hexpand (GTK_WIDGET (top), TRUE);

   self->browse.filter_box = GTK_BOX (filter_box);
   self->browse.filter_popover = popover;
  gtk_widget_set_margin_top (GTK_WIDGET (filter_box), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (filter_box), 6);
  gtk_widget_set_margin_start (GTK_WIDGET (filter_box), 6);
  gtk_widget_set_margin_end (GTK_WIDGET (filter_box), 6);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (filter_scroller),
                                  GTK_WIDGET (filter_box));
  /* Wide enough to read a category and its heading, and no taller than
     the popover can be: a long list scrolls instead of running off the
     screen. */
  gtk_widget_set_size_request (GTK_WIDGET (filter_scroller), 280, -1);
  gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (filter_scroller), 420);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (filter_scroller), TRUE);
  gtk_popover_set_child (GTK_POPOVER (popover), GTK_WIDGET (filter_scroller));

   self->browse_actions = GTK_BOX (top);
  g_object_ref_sink (top);

   self->browse.results_status = ADW_STATUS_PAGE (adw_status_page_new ());
   gtk_widget_set_vexpand (GTK_WIDGET (self->browse.results_status), TRUE);
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->browse.results_status),
                                 "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->browse.results_status),
                               _("Nothing to show yet"));
  gtk_widget_set_visible (GTK_WIDGET (self->browse.results_status), FALSE);

  self->browse.results_loading = ADW_SPINNER (adw_spinner_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->browse.results_loading), 64, 64);
  gtk_widget_set_valign (GTK_WIDGET (self->browse.results_loading), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (self->browse.results_loading), GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (GTK_WIDGET (self->browse.results_loading), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->browse.results_loading), TRUE);
  /* The list starts where the row above it ends: its first row is not
     rounded at the top, so the two read as one piece. */

   self->browse.results_group = group;
   self->browse.results_scroller = GTK_SCROLLED_WINDOW (scroller);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 clamped_group (group));
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_widget_set_vexpand (GTK_WIDGET (content), TRUE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->browse.results_loading));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->browse.results_status));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (scroller));
  gtk_widget_set_visible (GTK_WIDGET (scroller), FALSE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (content));

  {
    GtkActionBar *selection_bar = GTK_ACTION_BAR (gtk_action_bar_new ());
    GtkLabel *selection_label = GTK_LABEL (gtk_label_new (NULL));
    GtkButton *selection_button = GTK_BUTTON (gtk_button_new_with_label (_("Install")));

    gtk_widget_set_halign (GTK_WIDGET (selection_label), GTK_ALIGN_START);
    gtk_label_set_wrap (selection_label, TRUE);
    gtk_widget_set_margin_start (GTK_WIDGET (selection_label), 6);

    gtk_widget_add_css_class (GTK_WIDGET (selection_button), "suggested-action");
    gtk_widget_add_css_class (GTK_WIDGET (selection_button), "pill");
    gtk_widget_set_margin_end (GTK_WIDGET (selection_button), 6);

    gtk_action_bar_pack_start (selection_bar, GTK_WIDGET (selection_label));
    gtk_action_bar_pack_end (selection_bar, GTK_WIDGET (selection_button));
    gtk_action_bar_set_revealed (selection_bar, FALSE);

    self->browse.selection_bar = selection_bar;
    self->browse.selection_label = selection_label;
    self->browse.selection_button = selection_button;

    g_signal_connect_object (selection_button, "clicked",
                             G_CALLBACK (nuvashalauncher_content_page_on_batch_install_clicked),
                             self, 0);

    gtk_box_append (box, GTK_WIDGET (selection_bar));
  }

  /* The bottom of the list: a spinner while the next page is on its way
     and a retry row when one failed. Neither is a button the user has to
     press to see more: the list asks for the next page itself when it is
     scrolled to the end. */
  gtk_widget_set_halign (GTK_WIDGET (loading_more), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (loading_more), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (GTK_WIDGET (loading_more), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (loading_more), 12);
  gtk_widget_set_visible (GTK_WIDGET (loading_more), FALSE);
  gtk_box_append (GTK_BOX (loading_more), GTK_WIDGET (loading_spinner));
  gtk_box_append (GTK_BOX (loading_more), GTK_WIDGET (loading_label));
  adw_preferences_group_add (group, GTK_WIDGET (loading_more));

  gtk_widget_set_halign (GTK_WIDGET (retry_row), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (retry_row), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (GTK_WIDGET (retry_row), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (retry_row), 12);
  gtk_widget_set_visible (GTK_WIDGET (retry_row), FALSE);
  gtk_label_set_wrap (GTK_LABEL (retry_label), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (retry_label), "dim-label");
  gtk_box_append (GTK_BOX (retry_row), GTK_WIDGET (retry_label));
  gtk_box_append (GTK_BOX (retry_row), GTK_WIDGET (retry_button));
  adw_preferences_group_add (group, GTK_WIDGET (retry_row));

   self->browse.loading_more = GTK_BOX (loading_more);
   self->browse.retry_row = GTK_BOX (retry_row);
   self->browse.retry_label = GTK_LABEL (retry_label);

   g_signal_connect_object (retry_button, "clicked",
                            G_CALLBACK (nuvashalauncher_content_page_on_retry_page_clicked), self, 0);

  /* The list asks for the next page when it is scrolled to the end.
     GTK4 has no edge-reached signal on GtkScrolledWindow, so the
     adjustment is watched instead: when the user scrolls to the bottom,
     the value is at the upper bound. */
  {
    GtkAdjustment *adjustment =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));

     g_signal_connect_object (adjustment, "value-changed",
                              G_CALLBACK (nuvashalauncher_content_page_on_scroll_changed), self, 0);
  }

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack), GTK_WIDGET (box),
                                        "browse", _("Browse"),
                                      "system-search-symbolic");
}

/* ---------------------------------------------------------- *
 * The row that decides between Installed and Browse
 *
 * A row of the page, not a second header bar. A header bar here would
 * carry a title and a close button over content that has neither, and
 * would put two bars between the top of the tab and the list. It is a
 * plain box: the actions of the list being shown on the left, the
 * switcher on the right, because which list is shown is what the right
 * end of a toolbar row is for.
 * ---------------------------------------------------------- */

  /* Icon-only list buttons keep the toolbar to one line; tooltips identify
   * each view. */
static void
on_switcher_toggled (GtkToggleButton *button, gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);

  if (!gtk_toggle_button_get_active (button) || self->stack == NULL)
    return;

  adw_view_stack_set_visible_child_name (
    ADW_VIEW_STACK (self->stack),
    button == GTK_TOGGLE_BUTTON (self->switcher_browse) ? "browse"
                                                        : "installed");
}

static GtkWidget *
switcher_button (const char *icon_name, const char *label)
{
  GtkToggleButton *button = GTK_TOGGLE_BUTTON (gtk_toggle_button_new ());

  gtk_button_set_icon_name (GTK_BUTTON (button), icon_name);
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (button), label);
  gtk_widget_set_valign (GTK_WIDGET (button), GTK_ALIGN_CENTER);

  return GTK_WIDGET (button);
}

void
nuvashalauncher_content_page_build_bar (NuvashaLauncherContentPage *self)
{
  GtkBox *inner = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));

   self->bar = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  gtk_widget_add_css_class (GTK_WIDGET (self->bar), "nuvashalauncher-content-toolbar");
  gtk_widget_add_css_class (GTK_WIDGET (self->bar), "nuvashalauncher-compact-toolbar");
  gtk_widget_add_css_class (GTK_WIDGET (self->bar), "nuvashalauncher-page-actions");

  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);
    g_signal_connect_object (keys, "key-pressed",
                             G_CALLBACK (nuvashalauncher_content_page_on_content_search_key), self, 0);
  }

  /* Expand the spacer so the switcher stays at the right edge. */
  self->bar_spacer = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_set_hexpand (GTK_WIDGET (self->bar_spacer), TRUE);

  /* Use linked toggle buttons to keep this compact toolbar to one line. */
  self->switcher = g_object_ref_sink (GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0)));
  gtk_widget_add_css_class (GTK_WIDGET (self->switcher), "linked");

  self->switcher_installed = GTK_TOGGLE_BUTTON (
    switcher_button ("view-list-symbolic", _("Installed")));
  self->switcher_browse = GTK_TOGGLE_BUTTON (
    switcher_button ("web-browser-symbolic", _("Browse")));

  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->switcher_browse),
                               GTK_TOGGLE_BUTTON (self->switcher_installed));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->switcher_installed),
                                TRUE);

  g_signal_connect_object (self->switcher_installed, "toggled",
                           G_CALLBACK (on_switcher_toggled), self, 0);
  g_signal_connect_object (self->switcher_browse, "toggled",
                           G_CALLBACK (on_switcher_toggled), self, 0);

  gtk_box_append (GTK_BOX (self->switcher), GTK_WIDGET (self->switcher_installed));
  gtk_box_append (GTK_BOX (self->switcher), GTK_WIDGET (self->switcher_browse));

  gtk_widget_set_valign (GTK_WIDGET (self->switcher), GTK_ALIGN_CENTER);

  gtk_box_append (GTK_BOX (self->bar), GTK_WIDGET (self->bar_spacer));
  gtk_box_append (GTK_BOX (self->bar), GTK_WIDGET (self->switcher));

   gtk_widget_set_margin_start (GTK_WIDGET (self->bar), 0);
   gtk_widget_set_margin_end (GTK_WIDGET (self->bar), 0);

  /* Keep failures in a banner; use toasts for transient success messages. */
  self->banner = ADW_BANNER (adw_banner_new (""));
  adw_banner_set_button_label (ADW_BANNER (self->banner), _("Dismiss"));
  adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
  g_signal_connect_object (self->banner, "button-clicked",
                           G_CALLBACK (on_banner_dismiss), self, 0);

  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (self->banner));
  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (self->bar));
  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (self->stack));
  gtk_widget_set_vexpand (GTK_WIDGET (inner), TRUE);

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toasts),
                               GTK_WIDGET (inner));
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->toasts));
}

/* The actions of the list being shown go into the row, at its left, and
   the ones of the other list come out. A widget with no parent is the one
   that belongs here, because only one list holds them at a time.

   Browse brings its own row, whose search field takes the room; nothing
   has to push the switcher to the right for it. Installed has no such
   row, so the spacer does that. */
static void
remove_widget_from_parent (GtkWidget *widget)
{
  GtkWidget *parent = widget != NULL ? gtk_widget_get_parent (widget) : NULL;

  if (parent == NULL)
    return;
  if (ADW_IS_CLAMP (parent))
    adw_clamp_set_child (ADW_CLAMP (parent), NULL);
  else if (ADW_IS_HEADER_BAR (parent))
    adw_header_bar_remove (ADW_HEADER_BAR (parent), widget);
  else if (GTK_IS_BOX (parent))
    {
      GtkWidget *grandparent = gtk_widget_get_parent (parent);
      if (grandparent != NULL && ADW_IS_HEADER_BAR (grandparent))
        adw_header_bar_remove (ADW_HEADER_BAR (grandparent), widget);
      else
        gtk_box_remove (GTK_BOX (parent), widget);
    }
}

gboolean
nuvashalauncher_content_page_is_browsing (NuvashaLauncherContentPage *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_CONTENT_PAGE (self), FALSE);

  return self->stack != NULL &&
         g_strcmp0 (adw_view_stack_get_visible_child_name (self->stack), "browse") == 0;
}

gboolean
nuvashalauncher_content_page_is_installed_search_visible (NuvashaLauncherContentPage *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_CONTENT_PAGE (self), FALSE);

  if (nuvashalauncher_content_page_is_browsing (self))
    return self->browse.search_bar != NULL &&
           GTK_IS_WIDGET (self->browse.search_bar) &&
           gtk_widget_get_visible (GTK_WIDGET (self->browse.search_bar));

  return self->installed.search_entry != NULL &&
         GTK_IS_WIDGET (self->installed.search_entry) &&
         gtk_widget_get_visible (GTK_WIDGET (self->installed.search_entry));
}

static void
sync_header_mode (NuvashaLauncherContentPage *self)
{
  AdwHeaderBar *header = self->header_bar;
  gboolean browsing;

  if (header == NULL || self->stack == NULL)
    return;

  browsing = g_strcmp0 (adw_view_stack_get_visible_child_name (self->stack),
                        "browse") == 0;

  gtk_widget_set_visible (GTK_WIDGET (self->installed.search_button), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->bar), FALSE);

  if (!gtk_widget_is_ancestor (GTK_WIDGET (self->switcher), GTK_WIDGET (header)))
    {
      remove_widget_from_parent (GTK_WIDGET (self->switcher));
      adw_header_bar_pack_end (header, GTK_WIDGET (self->switcher));
    }

  if (browsing)
    {
      if (gtk_widget_get_parent (GTK_WIDGET (self->installed.search_bar)) != NULL &&
          gtk_widget_get_parent (GTK_WIDGET (self->installed.search_bar)) != GTK_WIDGET (self->header_search_clamp))
        remove_widget_from_parent (GTK_WIDGET (self->installed.search_bar));
      if (gtk_widget_get_parent (GTK_WIDGET (self->installed.search_bar)) == GTK_WIDGET (self->header_search_clamp))
        adw_clamp_set_child (self->header_search_clamp, NULL);

      if (gtk_widget_get_parent (GTK_WIDGET (self->browse.search_bar)) != GTK_WIDGET (self->header_search_clamp))
        {
          remove_widget_from_parent (GTK_WIDGET (self->browse.search_bar));
          adw_clamp_set_child (self->header_search_clamp, GTK_WIDGET (self->browse.search_bar));
        }
      gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp),
                              gtk_widget_get_visible (GTK_WIDGET (self->browse.search_bar)));
      adw_header_bar_set_title_widget (header, GTK_WIDGET (self->header_search_clamp));

      if (!gtk_widget_is_ancestor (GTK_WIDGET (self->installed.actions), GTK_WIDGET (header)))
        {
          remove_widget_from_parent (GTK_WIDGET (self->installed.actions));
          gtk_widget_set_hexpand (GTK_WIDGET (self->installed.actions), FALSE);
          adw_header_bar_pack_start (header, GTK_WIDGET (self->installed.actions));
        }
      if (self->installed.search_spacer != NULL)
        gtk_widget_set_visible (GTK_WIDGET (self->installed.search_spacer), FALSE);
      if (!gtk_widget_is_ancestor (GTK_WIDGET (self->browse_actions), GTK_WIDGET (header)))
        {
          remove_widget_from_parent (GTK_WIDGET (self->browse_actions));
          gtk_widget_set_hexpand (GTK_WIDGET (self->browse_actions), FALSE);
          adw_header_bar_pack_start (header, GTK_WIDGET (self->browse_actions));
        }
    }
  else
    {
      if (gtk_widget_get_parent (GTK_WIDGET (self->browse.search_bar)) != NULL &&
          gtk_widget_get_parent (GTK_WIDGET (self->browse.search_bar)) != GTK_WIDGET (self->header_search_clamp))
        remove_widget_from_parent (GTK_WIDGET (self->browse.search_bar));
      if (gtk_widget_get_parent (GTK_WIDGET (self->browse.search_bar)) == GTK_WIDGET (self->header_search_clamp))
        adw_clamp_set_child (self->header_search_clamp, NULL);

      if (gtk_widget_get_parent (GTK_WIDGET (self->installed.search_bar)) != GTK_WIDGET (self->header_search_clamp))
        {
          remove_widget_from_parent (GTK_WIDGET (self->installed.search_bar));
          adw_clamp_set_child (self->header_search_clamp, GTK_WIDGET (self->installed.search_bar));
          gtk_widget_set_hexpand (GTK_WIDGET (self->installed.search_bar), TRUE);
        }
      gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp),
                              gtk_widget_get_visible (GTK_WIDGET (self->installed.search_bar)));
      adw_header_bar_set_title_widget (header, GTK_WIDGET (self->header_search_clamp));

      remove_widget_from_parent (GTK_WIDGET (self->browse_actions));
      if (!gtk_widget_is_ancestor (GTK_WIDGET (self->installed.actions), GTK_WIDGET (header)))
        {
          remove_widget_from_parent (GTK_WIDGET (self->installed.actions));
          gtk_widget_set_hexpand (GTK_WIDGET (self->installed.actions), FALSE);
          adw_header_bar_pack_start (header, GTK_WIDGET (self->installed.actions));
        }
      if (self->installed.search_spacer != NULL)
        gtk_widget_set_visible (GTK_WIDGET (self->installed.search_spacer), FALSE);
    }
}

void
nuvashalauncher_content_page_attach_header (NuvashaLauncherContentPage *self,
                                           AdwHeaderBar *header)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_CONTENT_PAGE (self));
  g_return_if_fail (ADW_IS_HEADER_BAR (header));

  if (self->stack == NULL)
    return;

  if (self->header_bar == header)
    {
      sync_header_mode (self);
      return;
    }
  if (self->header_bar != NULL)
    nuvashalauncher_content_page_detach_header (self, self->header_bar);

  self->header_bar = header;
  if (self->header_search_clamp == NULL)
    {
      self->header_search_clamp = ADW_CLAMP (g_object_ref_sink (adw_clamp_new ()));
      adw_clamp_set_maximum_size (self->header_search_clamp, 520);
      gtk_widget_set_hexpand (GTK_WIDGET (self->header_search_clamp), TRUE);
      gtk_widget_set_margin_start (GTK_WIDGET (self->header_search_clamp), 6);
      gtk_widget_set_margin_end (GTK_WIDGET (self->header_search_clamp), 6);
    }

  sync_header_mode (self);
}

void
nuvashalauncher_content_page_detach_header (NuvashaLauncherContentPage *self,
                                           AdwHeaderBar *header)
{
  gboolean browsing;

  g_return_if_fail (NUVASHALAUNCHER_IS_CONTENT_PAGE (self));
  if (self->header_bar != header || header == NULL)
    return;

  self->header_bar = NULL;
  adw_header_bar_set_title_widget (header, NULL);
  if (self->stack == NULL)
    return;

  remove_widget_from_parent (GTK_WIDGET (self->installed.actions));
  gtk_widget_set_hexpand (GTK_WIDGET (self->installed.actions), TRUE);
  remove_widget_from_parent (GTK_WIDGET (self->browse_actions));
  gtk_widget_set_hexpand (GTK_WIDGET (self->browse_actions), TRUE);
  remove_widget_from_parent (GTK_WIDGET (self->switcher));
  if (gtk_widget_get_parent (GTK_WIDGET (self->switcher)) != GTK_WIDGET (self->bar))
    gtk_box_append (self->bar, GTK_WIDGET (self->switcher));
  gtk_widget_set_visible (GTK_WIDGET (self->bar), TRUE);
  if (self->bar_spacer != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->bar_spacer), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->installed.search_button), TRUE);
  if (self->installed.search_spacer != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->installed.search_spacer), TRUE);

  if (gtk_widget_get_parent (GTK_WIDGET (self->browse.search_bar)) != GTK_WIDGET (self->browse_actions))
    {
      remove_widget_from_parent (GTK_WIDGET (self->browse.search_bar));
      gtk_box_append (self->browse_actions, GTK_WIDGET (self->browse.search_bar));
    }

  if (gtk_widget_get_parent (GTK_WIDGET (self->installed.search_bar)) != GTK_WIDGET (self->installed.actions))
    {
      remove_widget_from_parent (GTK_WIDGET (self->installed.search_bar));
      gtk_box_prepend (self->installed.actions,
                       GTK_WIDGET (self->installed.search_bar));
      gtk_widget_set_hexpand (GTK_WIDGET (self->installed.search_bar),
                              gtk_widget_get_visible (GTK_WIDGET (self->installed.search_bar)));
    }

  browsing = self->stack != NULL &&
             g_strcmp0 (adw_view_stack_get_visible_child_name (self->stack), "browse") == 0;
  nuvashalauncher_content_page_move_actions_into_bar (self, browsing);
}

void
nuvashalauncher_content_page_move_actions_into_bar (NuvashaLauncherContentPage *self, gboolean browsing)
{
  if (self->header_bar != NULL)
    {
      sync_header_mode (self);
      return;
    }

  GtkWidget *wanted = browsing ? GTK_WIDGET (self->browse_actions)
                               : GTK_WIDGET (self->installed.actions);
  GtkWidget *other = browsing ? GTK_WIDGET (self->installed.actions)
                              : GTK_WIDGET (self->browse_actions);

  if (self->bar == NULL)
    return;

  if (other != NULL && gtk_widget_get_parent (other) == GTK_WIDGET (self->bar))
    gtk_box_remove (GTK_BOX (self->bar), other);

  /* At the front, which is the left end of the row: everything after it
     is the spacer and then the switcher. */
  if (wanted != NULL && gtk_widget_get_parent (wanted) != GTK_WIDGET (self->bar))
    gtk_box_insert_child_after (GTK_BOX (self->bar), wanted, NULL);

  /* Both action rows contain an expanding search entry, so the spacer is
     hidden while either action row is visible. */
   if (self->bar_spacer != NULL)
     gtk_widget_set_visible (GTK_WIDGET (self->bar_spacer), FALSE);
}
