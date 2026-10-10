/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-page-private.h"
#include "aml-i18n.h"
#include "aml-markdown.h"



/* One flat button with a symbolic icon, the way GNOME writes a toolbar
 * action: the label is the tooltip and the accessible name. */
static GtkWidget *
toolbar_button (const char *icon_name, const char *label, GCallback callback,
                gpointer user_data)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon_name);

  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (button, label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect (button, "clicked", callback, user_data);

  return button;
}


/* ---------------------------------------------------------- *
 * Building the page
 * ---------------------------------------------------------- */

void
aml_content_page_build_installed (AmlContentPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   GtkSearchEntry *search_bar = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  AdwPreferencesGroup *group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

   gtk_widget_set_hexpand (actions, TRUE);
   self->installed.search_bar = GTK_SEARCH_ENTRY (search_bar);
   self->installed.search_entry = GTK_SEARCH_ENTRY (search_bar);
   gtk_widget_set_hexpand (GTK_WIDGET (search_bar), TRUE);
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_bar),
                                            _("Search"));
     gtk_widget_set_visible (GTK_WIDGET (search_bar), TRUE);
   g_signal_connect (search_bar, "changed",
                     G_CALLBACK (aml_content_page_on_installed_search_changed), self);
     gtk_box_append (GTK_BOX (actions), GTK_WIDGET (search_bar));

   self->installed.update_all = GTK_BUTTON (toolbar_button (
     "software-update-available-symbolic", _("Update All"),
     G_CALLBACK (aml_content_page_on_update_all_clicked), self));
   gtk_widget_set_sensitive (GTK_WIDGET (self->installed.update_all), FALSE);
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->installed.update_all));
  gtk_box_append (GTK_BOX (actions),
                  toolbar_button ("folder-symbolic", _("Open Folder"),
                                  G_CALLBACK (aml_content_page_on_open_folder_clicked), self));
    self->browse.spinner = ADW_SPINNER (adw_spinner_new ());
   gtk_widget_set_valign (GTK_WIDGET (self->browse.spinner), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->browse.spinner), FALSE);
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->browse.spinner));

  /* The actions belong to the toolbar row above both lists, not to this
     one of them: they are the same row whichever list is shown, and
     moving them keeps a download in progress from being thrown away by a
     visit to the other list.

     The row of the other list is taken out of the bar when the lists are
     switched, so this one is held here: without it, taking it out would
     be the last reference to it and would destroy the box and the
     spinner inside it while self->browse.spinner still points at them. */
   self->installed.actions = GTK_BOX (actions);
  g_object_ref_sink (actions);

   self->installed.status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->installed.status),
                                 aml_content_type_icon (self->type));
  adw_status_page_set_title (ADW_STATUS_PAGE (self->installed.status),
                               _("Nothing installed yet"));
  adw_status_page_set_description (
      ADW_STATUS_PAGE (self->installed.status),
       _("Browse for something to put into the folder of this instance"));

  /* The list starts where the row above it ends: its first row is not
     rounded at the top, so the two read as one piece. */
  gtk_widget_add_css_class (GTK_WIDGET (group), "flush-top");

   self->installed.group = group;
   self->installed.scroller = GTK_SCROLLED_WINDOW (scroller);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 GTK_WIDGET (group));
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->installed.status));
  gtk_box_append (GTK_BOX (content), scroller);
  gtk_widget_set_visible (scroller, FALSE);
  gtk_box_append (GTK_BOX (box), content);

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack), box,
                                        "installed", _("Installed"),
                                      "view-list-symbolic");
}

/* The list of what Modrinth has for this instance is asked for when
 * Browse is looked at, not when the instance page is opened. */
/* Defined with the row it moves into, further down. */
void aml_content_page_move_actions_into_bar (AmlContentPage *self, gboolean browsing);

void
aml_content_page_on_stack_switched (GObject *stack, GParamSpec *pspec G_GNUC_UNUSED,
                   gpointer user_data)
{
  AmlContentPage *self = user_data;
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
  aml_content_page_move_actions_into_bar (self, browsing);

  /* Installed is a list of its own: the project page opened from Browse
     is left with the list it was picked from. */
  if (!browsing)
    {
      aml_content_page_clear_picks (self);
      aml_content_page_close_detail (self);
    }

  if (!browsing || self->browse.model->searched)
    return;

  self->browse.model->searched = TRUE;
  aml_content_page_run_search (self, FALSE);
}

void
aml_content_page_build_browse (AmlContentPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   GtkSearchEntry *search_bar = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  GtkWidget *filter_button = gtk_menu_button_new ();
  GtkWidget *popover = gtk_popover_new ();
  GtkWidget *filter_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *filter_scroller = gtk_scrolled_window_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *loading_more = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *loading_spinner = gtk_spinner_new ();
  GtkWidget *loading_label = gtk_label_new (_("Loading more…"));
  GtkWidget *retry_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *retry_label = gtk_label_new (NULL);
  GtkWidget *retry_button = gtk_button_new_with_label (_("Retry"));
  GtkWidget *foot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *pick_button = gtk_button_new ();
  GtkWidget *pick_icon = gtk_image_new_from_icon_name (
      "folder-download-symbolic");
  GtkWidget *pick_overlay = gtk_overlay_new ();
  GtkWidget *pick_badge = gtk_label_new (NULL);
  g_autofree char *pick_accessible_label = NULL;
  AdwPreferencesGroup *group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

   self->browse.search_entry = GTK_SEARCH_ENTRY (search_bar);
   self->browse.search_bar = GTK_SEARCH_ENTRY (search_bar);
  /* The field takes the room the row has, the way the modpack browser's
     search does, rather than a fixed width of its own. */
  gtk_widget_set_hexpand (GTK_WIDGET (search_bar), TRUE);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_bar),
                                            _("Search"));
  gtk_widget_set_visible (GTK_WIDGET (search_bar), TRUE);

  g_signal_connect (search_bar, "changed",
                    G_CALLBACK (aml_content_page_on_search_changed), self);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_BUBBLE);
    gtk_widget_add_controller (GTK_WIDGET (search_bar), keys);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (aml_content_page_on_search_key), self);
  }

  gtk_box_append (GTK_BOX (top), GTK_WIDGET (search_bar));

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (filter_button),
                                 "view-more-symbolic");
    gtk_widget_set_tooltip_text (filter_button, _("Filter by category"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (filter_button),
                                     GTK_ACCESSIBLE_PROPERTY_LABEL, _("Filter"),
                                  -1);
  gtk_widget_set_valign (filter_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (filter_button, "flat");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (filter_button), popover);
  /* A click opens the popover but does not emit "activate" on the
     menu button, so the load hangs off the popover's "show" instead. */
  g_signal_connect (popover, "show",
                    G_CALLBACK (aml_content_page_on_filter_clicked), self);
  gtk_box_append (GTK_BOX (top), filter_button);

  /* The row takes the room the bar has, the search inside it takes most
     of that, and the filter and the switcher sit at its right. */
  gtk_widget_set_hexpand (top, TRUE);

   self->browse.filter_box = GTK_BOX (filter_box);
  gtk_widget_set_margin_top (filter_box, 6);
  gtk_widget_set_margin_bottom (filter_box, 6);
  gtk_widget_set_margin_start (filter_box, 6);
  gtk_widget_set_margin_end (filter_box, 6);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (filter_scroller),
                                 filter_box);
  /* Wide enough to read a category and its heading, and no taller than
     the popover can be: a long list scrolls instead of running off the
     screen. */
  gtk_widget_set_size_request (filter_scroller, 280, -1);
  gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (filter_scroller), 420);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (filter_scroller), TRUE);
  gtk_popover_set_child (GTK_POPOVER (popover), filter_scroller);

   self->browse_actions = GTK_BOX (top);
  g_object_ref_sink (top);

   self->browse.results_status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->browse.results_status),
                                 "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->browse.results_status),
                               _("Nothing to show yet"));
  adw_status_page_set_description (
      ADW_STATUS_PAGE (self->browse.results_status),
        _("Type what to look for, or browse everything Modrinth has for this "
        "instance"));
  /* The list starts where the row above it ends: its first row is not
     rounded at the top, so the two read as one piece. */
  gtk_widget_add_css_class (GTK_WIDGET (group), "flush-top");

   self->browse.results_group = group;
   self->browse.results_scroller = GTK_SCROLLED_WINDOW (scroller);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 GTK_WIDGET (group));
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->browse.results_status));
  gtk_box_append (GTK_BOX (content), scroller);
  gtk_widget_set_visible (scroller, FALSE);
  gtk_box_append (GTK_BOX (box), content);

  /* The foot of the list: the button that installs everything the
     checkmarks of the versions have on, and nothing else. It is a plain
     download icon at the left end of the row, where the actions of the
     other list sit. */
  gtk_widget_add_css_class (pick_button, "flat");
  gtk_widget_set_valign (pick_button, GTK_ALIGN_CENTER);
   pick_accessible_label = g_strdup_printf (
       _("Install the selected %s?"), aml_content_type_title (self->type));
   gtk_accessible_update_property (GTK_ACCESSIBLE (pick_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   pick_accessible_label, -1);
  gtk_button_set_child (GTK_BUTTON (pick_button), pick_icon);
  g_signal_connect (pick_button, "clicked",
                    G_CALLBACK (aml_content_page_on_pick_button_clicked), self);

   /* Keep the count attached to the button without putting it inside the
      button's hit target. The overlay keeps it correctly positioned at
      every scale without changing the button's width. */
   gtk_widget_add_css_class (pick_badge, "aml-badge");
   gtk_widget_set_valign (pick_badge, GTK_ALIGN_START);
   gtk_widget_set_halign (pick_badge, GTK_ALIGN_END);
   gtk_widget_set_margin_top (pick_badge, 1);
   gtk_widget_set_margin_end (pick_badge, 1);

   gtk_overlay_set_child (GTK_OVERLAY (pick_overlay), pick_button);
   gtk_overlay_add_overlay (GTK_OVERLAY (pick_overlay), pick_badge);
   gtk_box_append (GTK_BOX (foot), pick_overlay);
  gtk_widget_set_margin_start (foot, 12);
  gtk_widget_set_margin_end (foot, 12);
  gtk_widget_set_margin_top (foot, 6);
  gtk_widget_set_margin_bottom (foot, 6);
  gtk_box_append (GTK_BOX (box), foot);

  self->pick_button = pick_button;
  self->pick_badge = pick_badge;

  /* Nothing is picked yet: the button starts where the list starts, with
     no number on it and nothing to install. */
  aml_content_page_refresh_picks (self);

  /* The bottom of the list: a spinner while the next page is on its way
     and a retry row when one failed. Neither is a button the user has to
     press to see more: the list asks for the next page itself when it is
     scrolled to the end. */
  gtk_widget_set_halign (loading_more, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (loading_more, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (loading_more, 12);
  gtk_widget_set_margin_bottom (loading_more, 12);
  gtk_widget_set_visible (loading_more, FALSE);
  gtk_box_append (GTK_BOX (loading_more), loading_spinner);
  gtk_box_append (GTK_BOX (loading_more), loading_label);
  adw_preferences_group_add (group, loading_more);

  gtk_widget_set_halign (retry_row, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (retry_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (retry_row, 12);
  gtk_widget_set_margin_bottom (retry_row, 12);
  gtk_widget_set_visible (retry_row, FALSE);
  gtk_label_set_wrap (GTK_LABEL (retry_label), TRUE);
  gtk_widget_add_css_class (retry_label, "dim-label");
  gtk_box_append (GTK_BOX (retry_row), retry_label);
  gtk_box_append (GTK_BOX (retry_row), retry_button);
  adw_preferences_group_add (group, retry_row);

   self->browse.loading_more = GTK_BOX (loading_more);
   self->browse.retry_row = GTK_BOX (retry_row);
   self->browse.retry_label = GTK_LABEL (retry_label);

  g_signal_connect (retry_button, "clicked",
                    G_CALLBACK (aml_content_page_on_retry_page_clicked), self);

  /* The list asks for the next page when it is scrolled to the end.
     GTK4 has no edge-reached signal on GtkScrolledWindow, so the
     adjustment is watched instead: when the user scrolls to the bottom,
     the value is at the upper bound. */
  {
    GtkAdjustment *adjustment =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));

    g_signal_connect (adjustment, "value-changed",
                      G_CALLBACK (aml_content_page_on_scroll_changed), self);
  }

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack), box,
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

/* The two buttons that show one of the lists. They carry the icons only:
 * the buttons have no words, so the row they share with the search field
 * stays one line at any width, and the tooltip is what says which list
 * each one shows. */
static void
on_switcher_toggled (GtkToggleButton *button, gpointer user_data)
{
  AmlContentPage *self = user_data;

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
  GtkWidget *button = gtk_toggle_button_new ();

  gtk_button_set_icon_name (GTK_BUTTON (button), icon_name);
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
     gtk_widget_set_tooltip_text (button, _(label));
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, _(label), -1);

  return button;
}

void
aml_content_page_build_bar (AmlContentPage *self)
{
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  self->bar = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));

  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);
    g_signal_connect (keys, "key-pressed",
                      G_CALLBACK (aml_content_page_on_content_search_key), self);
  }

  /* The actions go in before this and the switcher after it, so the
     switcher ends up at the right end of the row however many actions
     there are. Aligning inside a box would not move it: a box gives every
     child its own width. */
  self->bar_spacer = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_set_hexpand (GTK_WIDGET (self->bar_spacer), TRUE);

  /* Two buttons that behave as one choice, drawn as the linked pair of
     GNOME. A view switcher of libadwaita always carries the words of a
     page under its icon, and this row has no room for two lines. */
  self->switcher = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_add_css_class (GTK_WIDGET (self->switcher), "linked");

  self->switcher_installed = GTK_TOGGLE_BUTTON (
    switcher_button ("view-list-symbolic", "Installed"));
  self->switcher_browse = GTK_TOGGLE_BUTTON (
    switcher_button ("web-browser-symbolic", "Browse"));

  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->switcher_browse),
                               GTK_TOGGLE_BUTTON (self->switcher_installed));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->switcher_installed),
                                TRUE);

  g_signal_connect (self->switcher_installed, "toggled",
                    G_CALLBACK (on_switcher_toggled), self);
  g_signal_connect (self->switcher_browse, "toggled",
                    G_CALLBACK (on_switcher_toggled), self);

  gtk_box_append (GTK_BOX (self->switcher), GTK_WIDGET (self->switcher_installed));
  gtk_box_append (GTK_BOX (self->switcher), GTK_WIDGET (self->switcher_browse));

  gtk_widget_set_valign (GTK_WIDGET (self->switcher), GTK_ALIGN_CENTER);

  gtk_box_append (GTK_BOX (self->bar), GTK_WIDGET (self->bar_spacer));
  gtk_box_append (GTK_BOX (self->bar), GTK_WIDGET (self->switcher));

  gtk_widget_set_margin_top (GTK_WIDGET (self->bar), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->bar), 6);
  gtk_widget_set_margin_start (GTK_WIDGET (self->bar), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->bar), 12);

  /* A failure is said in a banner above the row, where it stays until it
     is read; what a toast says is an operation that just worked. */
  self->banner = ADW_BANNER (adw_banner_new (""));
  adw_banner_set_button_label (ADW_BANNER (self->banner), "Dismiss");
  adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
  g_signal_connect (self->banner, "button-clicked",
                    G_CALLBACK (on_banner_dismiss), self);

  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (self->banner));
  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (self->bar));
  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (self->stack));

  /* The list is the left half; the project that is opened is the right
     half, and is empty until one is opened. Collapsed shows one page at a
     time, which is the list alone. */
  self->project.split = ADW_NAVIGATION_SPLIT_VIEW (
    adw_navigation_split_view_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->project.split), TRUE);
  /* Named, so the stylesheet keeps the left half the colour of the page
     instead of the sidebar colour it would take on its own. */
  gtk_widget_add_css_class (GTK_WIDGET (self->project.split), "flat-split");
  adw_navigation_split_view_set_sidebar (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split),
    adw_navigation_page_new (inner, aml_content_type_title (self->type)));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), TRUE);
  adw_navigation_split_view_set_sidebar_width_fraction (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), 0.5);
  /* Half the page, rather than the narrow column a split view gives a
     sidebar by default: this half holds the search row and the list. */
  adw_navigation_split_view_set_min_sidebar_width (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), 360);
  adw_navigation_split_view_set_max_sidebar_width (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), 640);

  /* One overlay over the list and the project, so a message about either
     one is said in the same place. */
  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toasts),
                               GTK_WIDGET (self->project.split));
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->toasts));
}

/* The actions of the list being shown go into the row, at its left, and
   the ones of the other list come out. A widget with no parent is the one
   that belongs here, because only one list holds them at a time.

   Browse brings its own row, whose search field takes the room; nothing
   has to push the switcher to the right for it. Installed has no such
   row, so the spacer does that. */
void
aml_content_page_move_actions_into_bar (AmlContentPage *self, gboolean browsing)
{
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

   /* Both action rows contain an expanding search entry. The spacer used by
      the old Installed-only toolbar would consume that room and leave the
      entry at its natural width. */
   if (self->bar_spacer != NULL)
     gtk_widget_set_visible (GTK_WIDGET (self->bar_spacer), FALSE);
}
