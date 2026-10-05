/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The AML main window: header bar menus, the instance grid and the
 * empty state. Instances are shown exactly as AmlInstanceManager keeps
 * them; nothing here launches or downloads anything yet. */

#include "aml-main-window.h"
#include "aml-create-instance-dialog.h"
#include "aml-instance-page.h"
#include "aml-settings.h"

struct _AmlMainWindow
{
  AdwApplicationWindow parent_instance;

  AmlInstanceManager *manager;
  AmlSettings *settings;

  /* The navigation stack: the grid is its first page, the page of an
   * instance is pushed on top of it. */
  AdwNavigationView *navigation;

  /* Switches between the empty state and the instance grid. */
  GtkWidget *stack;
};

G_DEFINE_FINAL_TYPE (AmlMainWindow, aml_main_window, ADW_TYPE_APPLICATION_WINDOW)

/* Menu entries that exist from the start but stay disabled until their
 * feature is built: nothing may pretend to work. */
static const char *pending_actions[] = {
  "download-modpack", /* phase 13 */
  "shortcuts",
};

static void
on_create_instance (GSimpleAction *action G_GNUC_UNUSED,
                    GVariant      *parameter G_GNUC_UNUSED,
                    gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  GtkWidget *dialog = GTK_WIDGET (
    aml_create_instance_dialog_new (self->manager, self->settings));

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

/* What the row of a setting says when there is nothing to report. */
static const char *snapshots_subtitle =
  "Also list snapshots and the versions Mojang no longer serves";

/* Written the moment it is switched. When the file cannot be written
 * the setting keeps the value it had, the switch goes back to it and
 * the row says why: the dialog and the file never disagree. */
static void
on_show_snapshots_toggled (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  gboolean stored = aml_settings_get_show_snapshots (self->settings);
  g_autoptr (GError) error = NULL;

  if (active == stored)
    return;

  if (aml_settings_set_show_snapshots (self->settings, active, &error))
    {
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                                   snapshots_subtitle);
      return;
    }

  adw_switch_row_set_active (row, stored);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), error->message);
}

static void
on_settings (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  AdwDialog *dialog = adw_preferences_dialog_new ();
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *group = adw_preferences_group_new ();
  GtkWidget *row = adw_switch_row_new ();

  adw_dialog_set_title (dialog, "Settings");
  adw_preferences_dialog_set_search_enabled (ADW_PREFERENCES_DIALOG (dialog),
                                             FALSE);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), "Versions");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                         "What the launcher lists when an "
                                         "instance is created or edited.");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Show Snapshots");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), snapshots_subtitle);
  adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                             aml_settings_get_show_snapshots (self->settings));

  /* Connected after the value was set, so opening the dialog never
   * writes anything by itself. */
  g_signal_connect (row, "notify::active",
                    G_CALLBACK (on_show_snapshots_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (group));
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog),
                              ADW_PREFERENCES_PAGE (page));

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_about (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  AdwDialog *dialog = adw_about_dialog_new ();
  const char *app_ids[] = { "io.github.Flia7.AML", NULL };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), "AML");
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "Flia");
  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog),
                                "https://github.com/Flia7");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog),
                                  "Copyright (C) 2026 Flia");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog),
                                     GTK_LICENSE_GPL_3_0_ONLY);
  adw_about_dialog_add_credit_section (ADW_ABOUT_DIALOG (dialog),
                                       "Application ID", app_ids);
  adw_dialog_set_title (dialog, "About AML");

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static const GActionEntry window_entries[] = {
  { .name = "create-instance", .activate = on_create_instance },
  { .name = "download-modpack" },
  { .name = "settings", .activate = on_settings },
  { .name = "shortcuts" },
  { .name = "about", .activate = on_about },
};

static GMenu *
create_add_menu (void)
{
  GMenu *menu = g_menu_new ();

  g_menu_append (menu, "Create Instance", "win.create-instance");
  g_menu_append (menu, "Download Modpack", "win.download-modpack");

  return menu;
}

static GMenu *
create_app_menu (void)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) general = g_menu_new ();
  g_autoptr (GMenu) rest = g_menu_new ();

  g_menu_append (general, "Settings", "win.settings");
  g_menu_append (general, "Keyboard Shortcuts", "win.shortcuts");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (general));

  g_menu_append (rest, "About AML", "win.about");
  g_menu_append (rest, "Quit", "app.quit");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (rest));

  return g_steal_pointer (&menu);
}

/* GtkPopoverMenu builds its item widgets itself and ignores any
 * "tooltip-text" item attribute, so the tooltip of the disabled entry
 * is attached to the item widget after the popover was built. */
static void
set_menu_item_tooltip (GtkWidget  *root,
                       const char *label_text,
                       const char *tooltip)
{
  for (GtkWidget *child = gtk_widget_get_first_child (root);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      if (GTK_IS_LABEL (child) &&
          g_strcmp0 (gtk_label_get_text (GTK_LABEL (child)), label_text) == 0)
        {
          /* The label sits directly inside the menu item. */
          GtkWidget *item = gtk_widget_get_parent (child);

          if (item != NULL)
            gtk_widget_set_tooltip_text (item, tooltip);

          return;
        }

      set_menu_item_tooltip (child, label_text, tooltip);
    }
}

/* Opens the page of the instance this card shows. The id is put on the
 * button when the card is bound, so a card that lost its instance
 * simply does nothing. */
static void
on_card_clicked (GtkButton *button, gpointer user_data)
{
  AmlMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (button), "aml-instance-id");
  AmlInstance *instance = NULL;

  if (id == NULL)
    return;

  instance = aml_instance_manager_get_by_id (self->manager, id);

  if (instance == NULL)
    return;

  adw_navigation_view_push (
    self->navigation,
    ADW_NAVIGATION_PAGE (aml_instance_page_new (self->manager, instance,
                                                self->settings)));
}

/* One card of the instance grid. The button holds everything but Play,
 * so clicking the card opens the instance page; the children are
 * appended in this order and bind_card() walks them in the same
 * order. */
static GtkWidget *
create_card (AmlMainWindow *self)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *open = gtk_button_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *icon = gtk_image_new_from_icon_name ("applications-games-symbolic");
  GtkWidget *name = gtk_label_new (NULL);
  GtkWidget *version = gtk_label_new (NULL);
  GtkWidget *play = gtk_button_new_with_label ("Play");

  gtk_widget_set_margin_top (card, 12);
  gtk_widget_set_margin_bottom (card, 12);
  gtk_widget_set_margin_start (card, 12);
  gtk_widget_set_margin_end (card, 12);

  /* Instance icons are not part of this phase, so every card shows the
   * same square placeholder. */
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 96);
  gtk_widget_set_size_request (icon, 96, 96);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);

  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (name), 16);
  gtk_widget_set_halign (name, GTK_ALIGN_CENTER);

  gtk_label_set_ellipsize (GTK_LABEL (version), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (version), 16);
  gtk_widget_set_halign (version, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (version, "dim-label");

  /* Launching comes with phase 9: until then the button says so
   * instead of pretending to start the game. */
  gtk_widget_set_sensitive (play, FALSE);
  gtk_widget_set_tooltip_text (play, "Available in a later version");

  gtk_box_append (GTK_BOX (content), icon);
  gtk_box_append (GTK_BOX (content), name);
  gtk_box_append (GTK_BOX (content), version);

  gtk_button_set_child (GTK_BUTTON (open), content);
  gtk_widget_set_hexpand (open, TRUE);
  g_signal_connect (open, "clicked", G_CALLBACK (on_card_clicked), self);

  gtk_box_append (GTK_BOX (card), open);
  gtk_box_append (GTK_BOX (card), play);

  return card;
}

static void
setup_card (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
            gpointer                  item,
            gpointer                  user_data)
{
  gtk_list_item_set_child (GTK_LIST_ITEM (item), create_card (user_data));
}

static void
bind_card (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
           gpointer                  item,
           gpointer                  user_data G_GNUC_UNUSED)
{
  GtkListItem *list_item = GTK_LIST_ITEM (item);
  AmlInstance *instance = AML_INSTANCE (gtk_list_item_get_item (list_item));
  g_autofree char *name = NULL;
  g_autofree char *minecraft_version = NULL;
  g_autofree char *subtitle = NULL;
  AmlLoader loader = AML_LOADER_VANILLA;
  GtkWidget *card = NULL;
  GtkWidget *open = NULL;
  GtkWidget *content = NULL;
  GtkWidget *icon = NULL;
  GtkWidget *name_label = NULL;
  GtkWidget *version_label = NULL;
  const char *loader_name = NULL;

  if (instance == NULL)
    return;

  g_object_get (instance,
                "name", &name,
                "minecraft-version", &minecraft_version,
                "loader", &loader,
                NULL);

  loader_name = aml_loader_to_string (loader);
  if (loader_name != NULL && loader_name[0] != '\0')
    {
      g_autofree char *capitalized = g_strdup (loader_name);

      /* "fabric" is written as "Fabric" in the card. */
      capitalized[0] = g_ascii_toupper (capitalized[0]);
      subtitle = g_strdup_printf ("%s %s", minecraft_version, capitalized);
    }
  else
    subtitle = g_strdup (minecraft_version);

  card = gtk_list_item_get_child (list_item);
  open = gtk_widget_get_first_child (card);
  content = gtk_widget_get_first_child (open);
  icon = gtk_widget_get_first_child (content);
  name_label = gtk_widget_get_next_sibling (icon);
  version_label = gtk_widget_get_next_sibling (name_label);

  gtk_label_set_text (GTK_LABEL (name_label), name);
  gtk_label_set_text (GTK_LABEL (version_label), subtitle);

  g_object_set_data_full (G_OBJECT (open), "aml-instance-id",
                          g_strdup (aml_instance_get_id (instance)), g_free);
}

static void
update_visible_page (AmlMainWindow *self)
{
  GListStore *instances = aml_instance_manager_get_instances (self->manager);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (instances));

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack),
                                    n_items > 0 ? "instances" : "empty");
}

static void
on_instances_changed (AmlMainWindow *self,
                      GListModel    *model G_GNUC_UNUSED,
                      guint          position G_GNUC_UNUSED,
                      guint          removed G_GNUC_UNUSED,
                      guint          added G_GNUC_UNUSED)
{
  update_visible_page (self);
}

static void
aml_main_window_init (AmlMainWindow *self)
{
  g_autoptr (GMenu) add_menu = create_add_menu ();
  g_autoptr (GMenu) app_menu = create_app_menu ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *add_button = gtk_menu_button_new ();
  GtkWidget *users_button = gtk_menu_button_new ();
  GtkWidget *app_menu_button = gtk_menu_button_new ();
  GtkWidget *users_child = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *users_popover = gtk_popover_new ();
  GtkWidget *users_hint = gtk_label_new ("No accounts yet");
  GtkWidget *empty_page = adw_status_page_new ();
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *add_popover = NULL;

  g_action_map_add_action_entries (G_ACTION_MAP (self), window_entries,
                                   G_N_ELEMENTS (window_entries), self);

  /* Present in the menus, disabled until the feature exists. */
  for (gsize i = 0; i < G_N_ELEMENTS (pending_actions); i++)
    {
      GAction *action =
        g_action_map_lookup_action (G_ACTION_MAP (self), pending_actions[i]);

      g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);
    }

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
                           "Available in a later version");

  /* End of the header: Users, then the application menu. The window
   * controls are drawn by the header bar after them. */
  gtk_box_append (GTK_BOX (users_child),
                  gtk_image_new_from_icon_name ("avatar-default-symbolic"));
  gtk_box_append (GTK_BOX (users_child), gtk_label_new ("Users"));
  gtk_menu_button_set_child (GTK_MENU_BUTTON (users_button), users_child);

  gtk_widget_set_margin_start (users_hint, 12);
  gtk_widget_set_margin_end (users_hint, 12);
  gtk_widget_set_margin_top (users_hint, 12);
  gtk_widget_set_margin_bottom (users_hint, 12);
  gtk_popover_set_child (GTK_POPOVER (users_popover), users_hint);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (users_button), users_popover);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (app_menu_button),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app_menu_button),
                                  G_MENU_MODEL (app_menu));

  adw_header_bar_pack_end (ADW_HEADER_BAR (header), users_button);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app_menu_button);

  /* The empty state has no button in the middle: the "+" menu is the
   * only way to create an instance. */
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), "No Instances Yet");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty_page),
                                   "Use the + button in the header to create one.");
  gtk_widget_set_hexpand (empty_page, TRUE);
  gtk_widget_set_vexpand (empty_page, TRUE);

  self->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->stack), empty_page, "empty");
  gtk_widget_set_hexpand (self->stack, TRUE);
  gtk_widget_set_vexpand (self->stack, TRUE);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), self->stack);

  self->navigation = ADW_NAVIGATION_VIEW (adw_navigation_view_new ());

  /* The grid is the first page and stays in the view for good; the
   * page of an instance is pushed on top of it and dropped again. */
  adw_navigation_view_add (self->navigation,
                           adw_navigation_page_new_with_tag (toolbar, "AML",
                                                             "home"));

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      GTK_WIDGET (self->navigation));

  gtk_window_set_title (GTK_WINDOW (self), "AML");
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 640);
}

AmlMainWindow *
aml_main_window_new (GtkApplication *application, AmlInstanceManager *manager)
{
  g_return_val_if_fail (GTK_IS_APPLICATION (application), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (manager), NULL);

  AmlMainWindow *self = g_object_new (AML_TYPE_MAIN_WINDOW,
                                      "application", application,
                                      NULL);
  GListStore *instances = aml_instance_manager_get_instances (manager);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  GtkNoSelection *selection = NULL;
  GtkWidget *grid = NULL;
  GtkWidget *scroll = NULL;

  self->manager = g_object_ref (manager);
  self->settings = aml_settings_new ();

  g_signal_connect (factory, "setup", G_CALLBACK (setup_card), self);
  g_signal_connect (factory, "bind", G_CALLBACK (bind_card), self);

  /* gtk_no_selection_new() and gtk_grid_view_new() take ownership of
   * the references they get, so both references are created here and
   * handed over. The store itself stays owned by the manager. */
  selection = gtk_no_selection_new (g_object_ref (G_LIST_MODEL (instances)));
  grid = gtk_grid_view_new (GTK_SELECTION_MODEL (selection),
                            GTK_LIST_ITEM_FACTORY (factory));
  gtk_grid_view_set_min_columns (GTK_GRID_VIEW (grid), 1);
  gtk_grid_view_set_max_columns (GTK_GRID_VIEW (grid), 5);

  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), grid);
  gtk_widget_set_hexpand (scroll, TRUE);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_stack_add_named (GTK_STACK (self->stack), scroll, "instances");

  /* The connection goes away with the window, which outlives every
   * callback it could receive. */
  g_signal_connect_object (instances, "items-changed",
                           G_CALLBACK (on_instances_changed), self,
                           G_CONNECT_SWAPPED);
  update_visible_page (self);

  return self;
}

static void
aml_main_window_dispose (GObject *object)
{
  AmlMainWindow *self = AML_MAIN_WINDOW (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (aml_main_window_parent_class)->dispose (object);
}

static void
aml_main_window_class_init (AmlMainWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_main_window_dispose;
}
