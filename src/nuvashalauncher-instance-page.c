/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherInstancePage: settings, content, and logs for one instance.
 *
 * The settings page is where the instance itself is edited: every edit
 * Settings are persisted through nuvashalauncherInstanceManager. Content pages share
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
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-task.h"
#include "nuvashalauncher-operation.h"

G_DEFINE_FINAL_TYPE (nuvashalauncherInstancePage, nuvashalauncher_instance_page,
                     GTK_TYPE_BOX)

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

char *
instance_string (nuvashalauncherInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

/* "fabric" is shown as "Fabric", the same way the instance list writes it. */
char *
loader_display_name (nuvashalauncherInstance *instance)
{
  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;
  const char *nick = NULL;

  g_object_get (instance, "loader", &loader, NULL);
  nick = nuvashalauncher_loader_to_string (loader);

  if (nick == NULL || nick[0] == '\0')
     return g_strdup (_("Unknown"));

  char *name = g_strdup (nick);
  name[0] = g_ascii_toupper (name[0]);
  return name;
}

void
instance_page_show_toast (nuvashalauncherInstancePage *self, const char *message)
{
  nuvashalauncher_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toast_overlay), message);
}

/* The list binds an item when it enters the store, so a row that was
 * already bound keeps what it shows: the instance has to leave the store
 * and come back at the same place for the row to be bound again. The
 * order of the rows does not change. */
void
refresh_in_list (nuvashalauncherInstancePage *self)
{
  GListStore *store = nuvashalauncher_instance_manager_get_instances (self->manager);
  guint position = 0;
  gpointer item = NULL;

  if (!g_list_store_find (store, self->instance, &position))
    return;

  /* The instance leaves and comes back in one change, not two: a plain
     remove emits items-changed while the instance is out of the store,
     and the window closes the tab of an instance it cannot find, which
     destroys this page while this function is still running. */
  item = self->instance;
  g_list_store_splice (store, position, 1, &item, 1);
}

/* The content pages read the loader and the Minecraft version of the
 * instance when they are built, so changing either of them has to build
 * them again: the Mods page in particular keeps saying "No mod loader"
 * until it is built for the loader the instance now runs. */
void
rebuild_content_pages (nuvashalauncherInstancePage *self)
{
  static const struct {
    nuvashalauncherContentType type;
    const char *name;
    const char *title;
    const char *icon;
  } pages[] = {
    { nuvashalauncher_CONTENT_RESOURCE_PACK, "resource-packs", "Resource Packs",
      "image-x-generic-symbolic" },
    { nuvashalauncher_CONTENT_SHADER_PACK, "shader-packs", "Shader Packs",
      "applications-graphics-symbolic" },
    { nuvashalauncher_CONTENT_MOD, "mods", "Mods", "view-list-symbolic" },
  };
  g_autofree char *visible = NULL;

  if (self->stack == NULL)
    return;

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
                                        "edit-clear-all-symbolic");
    }

  if (visible != NULL)
    adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (self->stack),
                                           visible);
}

static void
on_content_tab_switched (GObject *stack, GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer user_data)
{
  (void) user_data;
  const char *name = adw_view_stack_get_visible_child_name (
    ADW_VIEW_STACK (stack));
  GtkWidget *child = adw_view_stack_get_child_by_name (
    ADW_VIEW_STACK (stack), name);

  if (child != NULL && nuvashalauncher_IS_CONTENT_PAGE (child))
    nuvashalauncher_content_page_reset_for_tab_switch (nuvashalauncher_CONTENT_PAGE (child));
}

static void
on_instance_page_width_changed (GObject *object,
                                GParamSpec *pspec G_GNUC_UNUSED,
                                gpointer user_data G_GNUC_UNUSED)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (object);
  int width = gtk_widget_get_width (GTK_WIDGET (self));

  if (self->switcher == NULL || width <= 0)
    return;
  adw_view_switcher_set_policy (
    self->switcher,
    width < 700 ? ADW_VIEW_SWITCHER_POLICY_NARROW
                : ADW_VIEW_SWITCHER_POLICY_WIDE);
}

/* Settings-page implementation lives in nuvashalauncher-instance-page-settings.c. */

/* The Directory row follows the launch: a running game makes both of its
   buttons insensitive, and stopping it makes them answer again. The
   loader cannot be changed under a running game either, so both of its
   rows follow the same state. */
static void
on_page_launcher_state (nuvashalauncher *launcher G_GNUC_UNUSED,
                        nuvashalauncherLaunchState state, const char *detail G_GNUC_UNUSED,
                        gpointer user_data)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);

  update_directory_row (self);

  if (self->loader_row != NULL)
    nuvashalauncher_loader_row_set_blocked (
      self->loader_row,
      nuvashalauncher_launch_state_is_busy (state));
}

nuvashalauncherInstancePage *
nuvashalauncher_instance_page_new (nuvashalauncherInstanceManager *manager, nuvashalauncherInstance *instance,
                       nuvashalauncherSettings *settings, nuvashalauncher *launcher)
{
  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE_MANAGER (manager), NULL);
  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (nuvashalauncher_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (nuvashalauncher_IS_LAUNCHER (launcher), NULL);

  nuvashalauncherInstancePage *self = g_object_new (nuvashalauncher_TYPE_INSTANCE_PAGE, NULL);
  AdwViewStack *stack = ADW_VIEW_STACK (adw_view_stack_new ());
  AdwViewSwitcher *switcher = ADW_VIEW_SWITCHER (adw_view_switcher_new ());
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  AdwNavigationView *settings_navigation;
  AdwNavigationPage *settings_page;
  g_autofree char *name = instance_string (instance, "name");

  self->manager = g_object_ref (manager);
  self->instance = g_object_ref (instance);
  self->settings = g_object_ref (settings);
  self->launcher = g_object_ref (launcher);
  self->switcher = switcher;
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
    GTK_WIDGET (nuvashalauncher_content_page_new (instance, nuvashalauncher_CONTENT_RESOURCE_PACK)),
     "resource-packs", _("Resource Packs"), "image-x-generic-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvashalauncher_content_page_new (instance, nuvashalauncher_CONTENT_SHADER_PACK)),
     "shader-packs", _("Shader Packs"), "applications-graphics-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvashalauncher_content_page_new (instance, nuvashalauncher_CONTENT_MOD)),
     "mods", _("Mods"), "view-list-symbolic");

  /* The logs of the instance are the logs of the launcher the window
     shares with the row, so the game can be watched from here. */
  self->logs_page = nuvashalauncher_logs_page_new (launcher);
  g_object_ref (self->logs_page);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack),
                                        GTK_WIDGET (self->logs_page), "logs", _("Logs"),
                                       "utilities-terminal-symbolic");

  /* Kept so that the content pages can be built again when the loader or
     the Minecraft version changes. */
  g_signal_connect_object (stack, "notify::visible-child",
                           G_CALLBACK (on_content_tab_switched), self, 0);

  /* The header holds only this switcher. There is no back button here:
   the dialog is named after the instance and its close button is what
   leaves it. */
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher),
                                ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher),
                                ADW_VIEW_STACK (stack));
  adw_header_bar_set_title_widget (header, GTK_WIDGET (switcher));
  self->stack = stack;
  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toast_overlay, GTK_WIDGET (stack));
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (toolbar));
  adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (header));
  adw_toolbar_view_set_content (toolbar, GTK_WIDGET (self->toast_overlay));
  gtk_widget_set_vexpand (GTK_WIDGET (toolbar), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (toolbar), TRUE);

  /* The dialog holds this box, and the box holds the toolbar view of the
     instance: there is no navigation page and no back button between the
     switcher and the five pages of the instance. */
   gtk_accessible_update_property (GTK_ACCESSIBLE (self),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);

  return self;
}

void
nuvashalauncher_instance_page_show_logs (nuvashalauncherInstancePage *self)
{
  g_return_if_fail (nuvashalauncher_IS_INSTANCE_PAGE (self));

  if (self->stack != NULL)
    adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (self->stack), "logs");
}

static void
nuvashalauncher_instance_page_dispose (GObject *object)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (object);

  /* The widget tree may be freed while the page object is kept alive
   * by a running operation: never touch it afterwards. The manager and
   * the instance stay, so those operations can still finish. */
  self->toast_overlay = NULL;
  self->icon_row = NULL;
  self->icon_path_label = NULL;
  self->stack = NULL;
  g_clear_object (&self->logs_page);
  self->java_row = NULL;
  self->directory_row = NULL;
  self->directory_change = NULL;
  self->directory_path_label = NULL;
  self->directory_spinner = NULL;

  nuvashalauncher_operation_cancel (self->scope);
  nuvashalauncher_operation_unref (self->scope);
  self->scope = NULL;

  /* The Java warning's manifest read is this page's: it stops with it. */
  if (self->versions_operation != NULL)
    {
      nuvashalauncher_operation_cancel (self->versions_operation);
      nuvashalauncher_operation_unref (self->versions_operation);
      self->versions_operation = NULL;
    }

  /* The loader rows are the page's. Their handlers go with them, so the
   * rows the widget tree may still hold cannot call back into a page
   * that is on its way out. */
  g_clear_object (&self->loader_row);

  G_OBJECT_CLASS (nuvashalauncher_instance_page_parent_class)->dispose (object);
}

static void
nuvashalauncher_instance_page_finalize (GObject *object)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);
  g_clear_object (&self->launcher);
  g_free (self->loader_version);
  g_free (self->export_name);
  g_free (self->export_summary);
  g_free (self->export_version);

  G_OBJECT_CLASS (nuvashalauncher_instance_page_parent_class)->finalize (object);
}

static void
nuvashalauncher_instance_page_class_init (nuvashalauncherInstancePageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = nuvashalauncher_instance_page_dispose;
  object_class->finalize = nuvashalauncher_instance_page_finalize;
}

static void
nuvashalauncher_instance_page_init (nuvashalauncherInstancePage *self)
{
  /* Everything is built in nuvashalauncher_instance_page_new(), which knows the
   * instance the page is for. */
  self->scope = nuvashalauncher_operation_new (NULL, NULL, NULL, NULL);
}
