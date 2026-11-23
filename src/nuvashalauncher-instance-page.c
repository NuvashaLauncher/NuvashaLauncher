/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherInstancePage: settings, content, and logs for one instance.
 *
 * The settings page is where the instance itself is edited: every edit
 * Settings are persisted through NuvashaLauncherInstanceManager. Content pages share
 * one implementation with different content types. Logs shows this instance's
 * game output. File operations use asynchronous GIO calls. */

#include "nuvashalauncher-instance-page-private.h"
#include "nuvashalauncher-i18n.h"

#include "nuvashalauncher-content-page.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-version-row.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-java-row.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-loader-row.h"
#include "nuvashalauncher-logs-page.h"
#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-servers-page.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-worlds-page.h"
#include "nuvashalauncher-task.h"

G_DEFINE_FINAL_TYPE (NuvashaLauncherInstancePage, nuvashalauncher_instance_page,
                     GTK_TYPE_BOX)

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

char *
instance_string (NuvashaLauncherInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

/* "fabric" is shown as "Fabric", the same way the instance list writes it. */
char *
loader_display_name (NuvashaLauncherInstance *instance)
{
  NuvashaLauncherLoader loader = NUVASHALAUNCHER_LOADER_VANILLA;
  const char *nick = NULL;

  g_object_get (instance, "loader", &loader, NULL);
  nick = nuvashalauncher_loader_to_string (loader);

  if (nick == NULL || nick[0] == '\0')
     return g_strdup (_("Unknown"));

  char *name = g_strdup (nick);
  name[0] = g_ascii_toupper (name[0]);
  return name;
}

GtkWidget *
instance_page_dialog_parent (NuvashaLauncherInstancePage *self)
{
  GtkWidget *host;
  GApplication *app;

  if (gtk_widget_get_root (GTK_WIDGET (self)) != NULL)
    return GTK_WIDGET (self);
  host = g_object_get_data (G_OBJECT (self), "nuvashalauncher-host-widget");
  if (GTK_IS_WIDGET (host))
    return host;
  app = g_application_get_default ();
  if (GTK_IS_APPLICATION (app))
    {
      GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
      if (win != NULL)
        return GTK_WIDGET (win);
    }
  return GTK_WIDGET (self);
}

void
instance_page_show_error (NuvashaLauncherInstancePage *self, const GError *error)
{
  if (self == NULL || self->toast_overlay == NULL)
    return;

  nuvashalauncher_ui_error_toast (instance_page_dialog_parent (self),
                                  GTK_WIDGET (self->toast_overlay), error);
}

void
instance_page_show_toast (NuvashaLauncherInstancePage *self, const char *message)
{
  if (self == NULL || self->toast_overlay == NULL)
    return;

  nuvashalauncher_ui_show_toast (instance_page_dialog_parent (self),
                                 GTK_WIDGET (self->toast_overlay), message);
}

static GtkWidget *
settings_page_widget (GtkWidget *page)
{
  if (page == NULL)
    return NULL;

  if (ADW_IS_NAVIGATION_VIEW (page))
    {
      AdwNavigationPage *nav_page =
        adw_navigation_view_get_visible_page (ADW_NAVIGATION_VIEW (page));
      GtkWidget *child = nav_page != NULL ? adw_navigation_page_get_child (nav_page) : NULL;

      return nuvashalauncher_settings_page_has_search (child) ? child : NULL;
    }

  return nuvashalauncher_settings_page_has_search (page) ? page : NULL;
}

static void
sync_sidebar_search_button (NuvashaLauncherInstancePage *self)
{
  GtkWidget *page = self->attached_header_page;
  GtkWidget *settings_widget = settings_page_widget (page);

  if (self->sidebar_search_button == NULL)
    return;

  self->syncing_search_button = TRUE;
  if (settings_widget != NULL)
    {
      gtk_widget_set_visible (GTK_WIDGET (self->sidebar_search_button), TRUE);
      gtk_toggle_button_set_active (
        self->sidebar_search_button,
        nuvashalauncher_settings_page_is_search_visible (settings_widget));
    }
  else if (page != NULL && NUVASHALAUNCHER_IS_CONTENT_PAGE (page))
    {
      NuvashaLauncherContentPage *cp = NUVASHALAUNCHER_CONTENT_PAGE (page);
      gtk_widget_set_visible (GTK_WIDGET (self->sidebar_search_button), TRUE);
      gtk_toggle_button_set_active (
        self->sidebar_search_button,
        nuvashalauncher_content_page_is_installed_search_visible (cp));
    }
  else if (page != NULL && NUVASHALAUNCHER_IS_LOGS_PAGE (page))
    {
      NuvashaLauncherLogsPage *lp = NUVASHALAUNCHER_LOGS_PAGE (page);
      gtk_widget_set_visible (GTK_WIDGET (self->sidebar_search_button), TRUE);
      gtk_toggle_button_set_active (
        self->sidebar_search_button,
        nuvashalauncher_logs_page_is_search_visible (lp));
    }
  else if (page != NULL && NUVASHALAUNCHER_IS_WORLDS_PAGE (page))
    {
      NuvashaLauncherWorldsPage *wp = NUVASHALAUNCHER_WORLDS_PAGE (page);
      gtk_widget_set_visible (GTK_WIDGET (self->sidebar_search_button), TRUE);
      gtk_toggle_button_set_active (
        self->sidebar_search_button,
        nuvashalauncher_worlds_page_is_search_visible (wp));
    }
  else if (page != NULL && NUVASHALAUNCHER_IS_SERVERS_PAGE (page))
    {
      NuvashaLauncherServersPage *sp = NUVASHALAUNCHER_SERVERS_PAGE (page);
      gtk_widget_set_visible (GTK_WIDGET (self->sidebar_search_button), TRUE);
      gtk_toggle_button_set_active (
        self->sidebar_search_button,
        nuvashalauncher_servers_page_is_search_visible (sp));
    }
  else
    {
      gtk_widget_set_visible (GTK_WIDGET (self->sidebar_search_button), FALSE);
      gtk_toggle_button_set_active (self->sidebar_search_button, FALSE);
    }
  self->syncing_search_button = FALSE;
}

static void
set_page_search_visible (GtkWidget *page, gboolean visible)
{
  GtkWidget *settings_widget = settings_page_widget (page);

  if (settings_widget != NULL)
    {
      nuvashalauncher_settings_page_set_search_visible (settings_widget, visible);
      return;
    }

  if (NUVASHALAUNCHER_IS_CONTENT_PAGE (page))
    nuvashalauncher_content_page_set_installed_search_visible (
      NUVASHALAUNCHER_CONTENT_PAGE (page), visible);
  else if (NUVASHALAUNCHER_IS_LOGS_PAGE (page))
    nuvashalauncher_logs_page_set_search_visible (
      NUVASHALAUNCHER_LOGS_PAGE (page), visible);
  else if (NUVASHALAUNCHER_IS_WORLDS_PAGE (page))
    nuvashalauncher_worlds_page_set_search_visible (
      NUVASHALAUNCHER_WORLDS_PAGE (page), visible);
  else if (NUVASHALAUNCHER_IS_SERVERS_PAGE (page))
    nuvashalauncher_servers_page_set_search_visible (
      NUVASHALAUNCHER_SERVERS_PAGE (page), visible);
}

static void
on_sidebar_search_toggled (GtkToggleButton *button, gpointer user_data)
{
  NuvashaLauncherInstancePage *self = NUVASHALAUNCHER_INSTANCE_PAGE (user_data);
  GtkWidget *page = self->attached_header_page;

  if (self->syncing_search_button || page == NULL)
    return;

  set_page_search_visible (page, gtk_toggle_button_get_active (button));
}

static void
on_content_page_subview_changed (GObject *obj G_GNUC_UNUSED,
                                 GParamSpec *pspec G_GNUC_UNUSED,
                                 gpointer user_data)
{
  sync_sidebar_search_button (NUVASHALAUNCHER_INSTANCE_PAGE (user_data));
}

static void
detach_header_page (NuvashaLauncherInstancePage *self)
{
  GtkWidget *page = self->attached_header_page;

  if (page == NULL || self->content_header == NULL)
    {
      self->attached_header_page = NULL;
      return;
    }

  self->attached_header_page = NULL;
  set_page_search_visible (page, FALSE);
  GtkWidget *title_widget = adw_header_bar_get_title_widget (self->content_header);
  if (title_widget != NULL)
    g_signal_handlers_disconnect_by_data (title_widget, self);

  GtkWidget *settings_widget = settings_page_widget (page);
  if (settings_widget != NULL)
    nuvashalauncher_settings_page_detach_header (settings_widget, self->content_header);
  else if (NUVASHALAUNCHER_IS_CONTENT_PAGE (page))
    nuvashalauncher_content_page_detach_header (
      NUVASHALAUNCHER_CONTENT_PAGE (page), self->content_header);
  else if (NUVASHALAUNCHER_IS_LOGS_PAGE (page))
    nuvashalauncher_logs_page_detach_header (
      NUVASHALAUNCHER_LOGS_PAGE (page), self->content_header);
  else if (NUVASHALAUNCHER_IS_WORLDS_PAGE (page))
    nuvashalauncher_worlds_page_detach_header (
      NUVASHALAUNCHER_WORLDS_PAGE (page), self->content_header);
  else if (NUVASHALAUNCHER_IS_SERVERS_PAGE (page))
    nuvashalauncher_servers_page_detach_header (
      NUVASHALAUNCHER_SERVERS_PAGE (page), self->content_header);
  adw_header_bar_set_title_widget (self->content_header, NULL);
}

static void
sync_header_to_visible_page (NuvashaLauncherInstancePage *self)
{
  GtkWidget *child;

  if (self->stack == NULL || self->content_header == NULL)
    return;

  child = adw_view_stack_get_visible_child (self->stack);
  if (child == self->attached_header_page)
    {
      sync_sidebar_search_button (self);
      return;
    }

  detach_header_page (self);
  if (child == NULL)
    {
      sync_sidebar_search_button (self);
      return;
    }

  GtkWidget *settings_widget = settings_page_widget (child);
  if (settings_widget != NULL)
    {
      nuvashalauncher_settings_page_attach_header (settings_widget, self->content_header);
      self->attached_header_page = child;
    }
  else if (NUVASHALAUNCHER_IS_CONTENT_PAGE (child))
    {
      nuvashalauncher_content_page_attach_header (
        NUVASHALAUNCHER_CONTENT_PAGE (child), self->content_header);
      self->attached_header_page = child;
    }
  else if (NUVASHALAUNCHER_IS_LOGS_PAGE (child))
    {
      nuvashalauncher_logs_page_attach_header (
        NUVASHALAUNCHER_LOGS_PAGE (child), self->content_header);
      self->attached_header_page = child;
    }
  else if (NUVASHALAUNCHER_IS_WORLDS_PAGE (child))
    {
      nuvashalauncher_worlds_page_attach_header (
        NUVASHALAUNCHER_WORLDS_PAGE (child), self->content_header);
      self->attached_header_page = child;
    }
  else if (NUVASHALAUNCHER_IS_SERVERS_PAGE (child))
    {
      nuvashalauncher_servers_page_attach_header (
        NUVASHALAUNCHER_SERVERS_PAGE (child), self->content_header);
      self->attached_header_page = child;
    }

  GtkWidget *title_widget = adw_header_bar_get_title_widget (self->content_header);
  if (title_widget != NULL)
    {
      g_signal_connect_object (title_widget, "notify::visible",
                               G_CALLBACK (on_content_page_subview_changed), self, 0);
      if (ADW_IS_CLAMP (title_widget))
        g_signal_connect_object (title_widget, "notify::child",
                                 G_CALLBACK (on_content_page_subview_changed), self, 0);
    }

  sync_sidebar_search_button (self);
}

/* The content pages read the loader and the Minecraft version of the
 * instance when they are built, so changing either of them has to build
 * them again: the Mods page in particular keeps saying "No mod loader"
 * until it is built for the loader the instance now runs. */
void
rebuild_content_pages (NuvashaLauncherInstancePage *self)
{
  static const struct {
    NuvashaLauncherContentType type;
    const char *name;
    const char *title;
    const char *icon;
  } pages[] = {
    { NUVASHALAUNCHER_CONTENT_RESOURCE_PACK, "resource-packs", "Resource Packs",
      "image-x-generic-symbolic" },
    { NUVASHALAUNCHER_CONTENT_SHADER_PACK, "shader-packs", "Shader Packs",
      "applications-graphics-symbolic" },
    { NUVASHALAUNCHER_CONTENT_MOD, "mods", "Mods", "package-x-generic-symbolic" },
  };
  g_autofree char *visible = NULL;

  if (self->stack == NULL)
    return;

  detach_header_page (self);

  visible = g_strdup (adw_view_stack_get_visible_child_name (
                        ADW_VIEW_STACK (self->stack)));

  for (guint i = 0; i < G_N_ELEMENTS (pages); i++)
    {
      GtkWidget *old = adw_view_stack_get_child_by_name (
        ADW_VIEW_STACK (self->stack), pages[i].name);

      if (old != NULL)
        adw_view_stack_remove (ADW_VIEW_STACK (self->stack), old);

      adw_view_stack_add_titled_with_icon (
        ADW_VIEW_STACK (self->stack),
        GTK_WIDGET (nuvashalauncher_content_page_new (self->instance, pages[i].type)),
         pages[i].name, _(pages[i].title), pages[i].icon);
    }

  /* A page that is added again lands at the end, after Logs. Logs is put
     back where it belongs, last, so the order of the switcher never
     changes: Settings, Resource Packs, Shader Packs, Mods, Logs. */
  if (self->logs_page != NULL)
    {
      adw_view_stack_remove (ADW_VIEW_STACK (self->stack),
                             GTK_WIDGET (self->logs_page));
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack),
                                            GTK_WIDGET (self->logs_page), "logs", _("Logs"),
                                            "utilities-terminal-symbolic");
    }

  if (visible != NULL)
    adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (self->stack),
                                           visible);
  sync_header_to_visible_page (self);
}

typedef struct {
  const char *name;
  const char *title;
  const char *icon;
  gboolean group_start; /* a line is drawn above, like Nautilus does */
} InstanceSidebarSection;

/* Settings, then the content, then the saves, then the logs; Stats is
 * pinned under the list. */
static const InstanceSidebarSection instance_sections[] = {
  { "settings", "Settings", "emblem-system-symbolic", FALSE },
  { "resource-packs", "Resource Packs", "image-x-generic-symbolic", TRUE },
  { "shader-packs", "Shader Packs", "applications-graphics-symbolic", FALSE },
  { "mods", "Mods", "package-x-generic-symbolic", FALSE },
  { "worlds", "Worlds", "checkbox-symbolic", TRUE },
  { "servers", "Servers", "network-server-symbolic", FALSE },
  { "logs", "Logs", "utilities-terminal-symbolic", TRUE },
};

static const InstanceSidebarSection stats_section =
  { "stats", "Stats", "power-profile-performance-symbolic", FALSE };

/* The line between two groups of rows. */
static void
sidebar_header_func (GtkListBoxRow *row, GtkListBoxRow *before G_GNUC_UNUSED,
                     gpointer user_data G_GNUC_UNUSED)
{
  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row),
                                          "nuvashalauncher-group-start")))
    {
      GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

      gtk_widget_set_margin_top (separator, 6);
      gtk_widget_set_margin_bottom (separator, 6);
      gtk_list_box_row_set_header (row, separator);
    }
  else
    gtk_list_box_row_set_header (row, NULL);
}

static void
sync_sidebar_to_page (NuvashaLauncherInstancePage *self, const char *name)
{
  if (self->sidebar_list == NULL || name == NULL)
    return;

  self->syncing_sidebar = TRUE;
  if (g_strcmp0 (name, stats_section.name) == 0 && self->stats_list != NULL)
    {
      gtk_list_box_unselect_all (self->sidebar_list);
      gtk_list_box_select_row (self->stats_list,
                               gtk_list_box_get_row_at_index (self->stats_list, 0));
    }
  else
    {
      if (self->stats_list != NULL)
        gtk_list_box_unselect_all (self->stats_list);
      for (int i = 0; ; i++)
        {
          GtkListBoxRow *row = gtk_list_box_get_row_at_index (self->sidebar_list, i);
          if (row == NULL)
            break;
          const char *row_name = g_object_get_data (G_OBJECT (row), "nuvashalauncher-page-name");
          if (g_strcmp0 (row_name, name) == 0)
            {
              gtk_list_box_select_row (self->sidebar_list, row);
              break;
            }
        }
    }
  self->syncing_sidebar = FALSE;
}

static void
on_sidebar_row_selected (GtkListBox    *box G_GNUC_UNUSED,
                         GtkListBoxRow *row,
                         gpointer       user_data)
{
  NuvashaLauncherInstancePage *self = NUVASHALAUNCHER_INSTANCE_PAGE (user_data);

  if (row == NULL || self->syncing_sidebar || self->stack == NULL)
    return;

  const char *name = g_object_get_data (G_OBJECT (row), "nuvashalauncher-page-name");
  if (name == NULL)
    return;

  adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (self->stack), name);
  if (self->split_view != NULL)
    adw_navigation_split_view_set_show_content (self->split_view, TRUE);
}

static GtkWidget *
create_sidebar_row (const InstanceSidebarSection *section)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon = gtk_image_new_from_icon_name (section->icon);
  GtkWidget *label = gtk_label_new (_(section->title));

  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), label);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-page-name",
                     (gpointer) section->name);
  g_object_set_data (G_OBJECT (row), "nuvashalauncher-group-start",
                     GINT_TO_POINTER (section->group_start));
  return row;
}

static void
on_content_tab_switched (GObject *stack, GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer user_data)
{
  NuvashaLauncherInstancePage *self = NUVASHALAUNCHER_INSTANCE_PAGE (user_data);
  const char *name = adw_view_stack_get_visible_child_name (
    ADW_VIEW_STACK (stack));
  GtkWidget *child = adw_view_stack_get_child_by_name (
    ADW_VIEW_STACK (stack), name);

  sync_sidebar_to_page (self, name);
  sync_header_to_visible_page (self);
  if (child != NULL && NUVASHALAUNCHER_IS_CONTENT_PAGE (child))
    nuvashalauncher_content_page_reset_for_tab_switch (NUVASHALAUNCHER_CONTENT_PAGE (child));
}

static void
on_instance_page_width_changed (GObject *object,
                                GParamSpec *pspec G_GNUC_UNUSED,
                                gpointer user_data G_GNUC_UNUSED)
{
  NuvashaLauncherInstancePage *self = NUVASHALAUNCHER_INSTANCE_PAGE (object);
  int width = gtk_widget_get_width (GTK_WIDGET (self));

  if (self->split_view == NULL || width <= 0)
    return;
  adw_navigation_split_view_set_collapsed (self->split_view, width < 600);
}

/* Settings-page implementation lives in nuvashalauncher-instance-page-settings.c. */

/* The Directory row follows the launch: a running game makes both of its
   buttons insensitive, and stopping it makes them answer again. The
   loader cannot be changed under a running game either, so both of its
   rows follow the same state. */
static void
on_page_launcher_state (NuvashaLauncher *launcher G_GNUC_UNUSED,
                        NuvashaLauncherLaunchState state, const char *detail G_GNUC_UNUSED,
                        gpointer user_data)
{
  NuvashaLauncherInstancePage *self = NUVASHALAUNCHER_INSTANCE_PAGE (user_data);

  update_directory_row (self);

  if (self->loader_row != NULL)
    nuvashalauncher_loader_row_set_blocked (
      self->loader_row,
      nuvashalauncher_launch_state_is_busy (state));
}

NuvashaLauncherInstancePage *
nuvashalauncher_instance_page_new (NuvashaLauncherInstanceManager *manager, NuvashaLauncherInstance *instance,
                       NuvashaLauncherSettings *settings, NuvashaLauncher *launcher)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_INSTANCE_MANAGER (manager), NULL);
  g_return_val_if_fail (NUVASHALAUNCHER_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (NUVASHALAUNCHER_IS_LAUNCHER (launcher), NULL);

  NuvashaLauncherInstancePage *self = g_object_new (NUVASHALAUNCHER_TYPE_INSTANCE_PAGE, NULL);
  AdwViewStack *stack = ADW_VIEW_STACK (adw_view_stack_new ());
  AdwNavigationSplitView *split_view =
    ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  AdwNavigationView *settings_navigation;
  AdwNavigationPage *settings_page;
  g_autofree char *name = instance_string (instance, "name");

  self->manager = g_object_ref (manager);
  self->instance = g_object_ref (instance);
  self->settings = g_object_ref (settings);
  self->launcher = g_object_ref (launcher);
  self->split_view = split_view;
  g_signal_connect_object (self, "notify::width",
                           G_CALLBACK (on_instance_page_width_changed), self, 0);
  settings_navigation = ADW_NAVIGATION_VIEW (adw_navigation_view_new ());
  settings_page = adw_navigation_page_new (create_settings_page (self),
                                           _("Settings"));
  adw_navigation_view_add (settings_navigation, settings_page);

  /* The Directory row cannot move files out from under a running game, so
     it follows the state of the launch the page shares with the row. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_page_launcher_state), self, 0);

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack),
                                       GTK_WIDGET (settings_navigation),
                                       "settings", _("Settings"),
                                        "emblem-system-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvashalauncher_content_page_new (instance, NUVASHALAUNCHER_CONTENT_RESOURCE_PACK)),
     "resource-packs", _("Resource Packs"), "image-x-generic-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvashalauncher_content_page_new (instance, NUVASHALAUNCHER_CONTENT_SHADER_PACK)),
     "shader-packs", _("Shader Packs"), "applications-graphics-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvashalauncher_content_page_new (instance, NUVASHALAUNCHER_CONTENT_MOD)),
     "mods", _("Mods"), "package-x-generic-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvashalauncher_worlds_page_new (instance, launcher)),
     "worlds", _("Worlds"), "checkbox-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvashalauncher_servers_page_new (instance, launcher)),
     "servers", _("Servers"), "network-server-symbolic");

  /* The logs of the instance are the logs of the launcher the window
     shares with the row, so the game can be watched from here. */
  self->logs_page = nuvashalauncher_logs_page_new (launcher);
  g_object_ref (self->logs_page);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack),
                                        GTK_WIDGET (self->logs_page), "logs", _("Logs"),
                                       "utilities-terminal-symbolic");

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), create_stats_page (self),
                                       stats_section.name, _("Stats"),
                                       stats_section.icon);

  /* Kept so that the content pages can be built again when the loader or
     the Minecraft version changes. */
  g_signal_connect_object (stack, "notify::visible-child",
                           G_CALLBACK (on_content_tab_switched), self, 0);

  /* Build the Nautilus-style left sidebar (navigation-sidebar list) and
   * content pane inside an AdwNavigationSplitView. */
  self->stack = stack;
  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toast_overlay, GTK_WIDGET (stack));

  GtkListBox *sidebar_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (sidebar_list), "navigation-sidebar");
  gtk_list_box_set_selection_mode (sidebar_list, GTK_SELECTION_SINGLE);
  for (guint i = 0; i < G_N_ELEMENTS (instance_sections); i++)
    gtk_list_box_append (sidebar_list, create_sidebar_row (&instance_sections[i]));
  g_signal_connect_object (sidebar_list, "row-selected",
                           G_CALLBACK (on_sidebar_row_selected), self, 0);
  gtk_list_box_set_header_func (sidebar_list, sidebar_header_func, NULL, NULL);
  self->sidebar_list = sidebar_list;

  GtkListBox *stats_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (stats_list), "navigation-sidebar");
  gtk_list_box_set_selection_mode (stats_list, GTK_SELECTION_SINGLE);
  gtk_list_box_append (stats_list, create_sidebar_row (&stats_section));
  g_signal_connect_object (stats_list, "row-selected",
                           G_CALLBACK (on_sidebar_row_selected), self, 0);
  self->stats_list = stats_list;

  GtkScrolledWindow *sidebar_scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (sidebar_scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (sidebar_scroll, GTK_WIDGET (sidebar_list));
  gtk_widget_set_vexpand (GTK_WIDGET (sidebar_scroll), TRUE);

  AdwHeaderBar *sidebar_header = ADW_HEADER_BAR (adw_header_bar_new ());
  adw_header_bar_set_show_title (sidebar_header, FALSE);
  GtkToggleButton *sidebar_search_button = GTK_TOGGLE_BUTTON (gtk_toggle_button_new ());
  gtk_button_set_icon_name (GTK_BUTTON (sidebar_search_button), "system-search-symbolic");
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (sidebar_search_button), _("Search"));
  gtk_widget_set_visible (GTK_WIDGET (sidebar_search_button), FALSE);
  g_signal_connect_object (sidebar_search_button, "toggled",
                           G_CALLBACK (on_sidebar_search_toggled), self, 0);
  adw_header_bar_pack_start (sidebar_header, GTK_WIDGET (sidebar_search_button));
  self->sidebar_search_button = sidebar_search_button;
  AdwToolbarView *sidebar_toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (sidebar_toolbar, GTK_WIDGET (sidebar_header));
  GtkWidget *sidebar_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (sidebar_box), GTK_WIDGET (sidebar_scroll));
  gtk_box_append (GTK_BOX (sidebar_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (sidebar_box), GTK_WIDGET (stats_list));
  adw_toolbar_view_set_content (sidebar_toolbar, sidebar_box);
  AdwNavigationPage *sidebar_nav_page = adw_navigation_page_new (
    GTK_WIDGET (sidebar_toolbar), name != NULL && *name != '\0' ? name : " ");

  AdwHeaderBar *content_header = ADW_HEADER_BAR (adw_header_bar_new ());
  adw_header_bar_set_centering_policy (content_header, ADW_CENTERING_POLICY_LOOSE);
  self->content_header = content_header;

  AdwToolbarView *content_toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (content_toolbar, GTK_WIDGET (content_header));
  adw_toolbar_view_set_content (content_toolbar, GTK_WIDGET (self->toast_overlay));
  AdwNavigationPage *content_nav_page = adw_navigation_page_new (
    GTK_WIDGET (content_toolbar), " ");
  self->content_nav_page = content_nav_page;

  adw_navigation_split_view_set_min_sidebar_width (split_view, 180);
  adw_navigation_split_view_set_max_sidebar_width (split_view, 240);
  adw_navigation_split_view_set_sidebar (split_view, sidebar_nav_page);
  adw_navigation_split_view_set_content (split_view, content_nav_page);
  gtk_widget_set_vexpand (GTK_WIDGET (split_view), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (split_view), TRUE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (split_view));

  sync_sidebar_to_page (self, "settings");
  sync_header_to_visible_page (self);

  /* The dialog holds this box, and the box holds the toolbar view of the
     instance: there is no navigation page and no back button between the
     switcher and the five pages of the instance. */
   gtk_accessible_update_property (GTK_ACCESSIBLE (self),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);

  return self;
}

void
nuvashalauncher_instance_page_show_logs (NuvashaLauncherInstancePage *self)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_INSTANCE_PAGE (self));

  if (self->stack != NULL)
    adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (self->stack), "logs");
}

static void
nuvashalauncher_instance_page_dispose (GObject *object)
{
  NuvashaLauncherInstancePage *self = NUVASHALAUNCHER_INSTANCE_PAGE (object);

  /* The widget tree may be freed while the page object is kept alive
   * by a running operation: never touch it afterwards. The manager and
   * the instance stay, so those operations can still finish. */
  detach_header_page (self);
  self->toast_overlay = NULL;
  self->icon_row = NULL;
  self->icon_path_label = NULL;
  self->stack = NULL;
  self->split_view = NULL;
  self->content_nav_page = NULL;
  self->content_header = NULL;
  self->sidebar_search_button = NULL;
  self->sidebar_list = NULL;
  self->stats_list = NULL;
  g_clear_object (&self->logs_page);
  self->java_row = NULL;
  self->directory_row = NULL;
  self->directory_change = NULL;
  self->directory_path_label = NULL;
  self->directory_spinner = NULL;

  if (self->scope != NULL)
    {
      nuvashalauncher_task_scope_cancel (self->scope);
      nuvashalauncher_task_scope_unref (self->scope);
      self->scope = NULL;
    }

  /* The Java warning's manifest read is this page's: it stops with it. */
  if (self->versions_operation != NULL)
    {
      nuvashalauncher_task_scope_cancel (self->versions_operation);
      nuvashalauncher_task_scope_unref (self->versions_operation);
      self->versions_operation = NULL;
    }

  if (self->export_download != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->export_download),
                                    (gpointer *) &self->export_download);
      self->export_download = NULL;
    }

  if (self->export_operation != NULL)
    {
      nuvashalauncher_task_scope_cancel (self->export_operation);
      nuvashalauncher_task_scope_unref (self->export_operation);
      self->export_operation = NULL;
    }

  /* The loader rows are the page's. Their handlers go with them, so the
   * rows the widget tree may still hold cannot call back into a page
   * that is on its way out. */
  g_clear_object (&self->loader_row);

  g_clear_object (&self->manager);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);
  g_clear_object (&self->launcher);

  G_OBJECT_CLASS (nuvashalauncher_instance_page_parent_class)->dispose (object);
}

static void
nuvashalauncher_instance_page_finalize (GObject *object)
{
  NuvashaLauncherInstancePage *self = NUVASHALAUNCHER_INSTANCE_PAGE (object);

  g_free (self->loader_version);
  g_free (self->export_name);
  g_free (self->export_summary);
  g_free (self->export_version);

  G_OBJECT_CLASS (nuvashalauncher_instance_page_parent_class)->finalize (object);
}

static void
nuvashalauncher_instance_page_class_init (NuvashaLauncherInstancePageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = nuvashalauncher_instance_page_dispose;
  object_class->finalize = nuvashalauncher_instance_page_finalize;
}

static void
nuvashalauncher_instance_page_init (NuvashaLauncherInstancePage *self)
{
  /* Everything is built in nuvashalauncher_instance_page_new(), which knows the
   * instance the page is for. */
  self->scope = nuvashalauncher_task_scope_new (NULL, NULL, NULL, NULL);
}
