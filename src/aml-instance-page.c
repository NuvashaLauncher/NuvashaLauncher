/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstancePage: the page of one instance.
 *
 * Settings is the only real page so far: every edit is written through
 * AmlInstanceManager, so what the page shows is what instances.json
 * holds. Resource Packs, Shader Packs, Mods and Logs say that they are
 * not available yet instead of pretending to work. Duplicating,
 * opening the folder and deleting run as asynchronous GIO operations
 * and never block the main loop. */

#include "aml-instance-page.h"

#include "aml-version-row.h"

struct _AmlInstancePage
{
  AdwNavigationPage parent_instance;

  AmlInstanceManager *manager;
  AmlInstance *instance;
  AmlSettings *settings;

  /* Borrowed from the widget tree of the page. Cleared in dispose(),
   * because the tree can go away while a running operation still keeps
   * this page alive. */
  GtkWidget *toast_overlay;
  GtkWidget *icon_row;
  GtkWidget *icon_image;
};

G_DEFINE_FINAL_TYPE (AmlInstancePage, aml_instance_page,
                     ADW_TYPE_NAVIGATION_PAGE)

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

static char *
instance_string (AmlInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

/* "fabric" is shown as "Fabric", the same way the grid card writes it. */
static char *
loader_display_name (AmlInstance *instance)
{
  AmlLoader loader = AML_LOADER_VANILLA;
  const char *nick = NULL;

  g_object_get (instance, "loader", &loader, NULL);
  nick = aml_loader_to_string (loader);

  if (nick == NULL || nick[0] == '\0')
    return g_strdup ("Unknown");

  char *name = g_strdup (nick);
  name[0] = g_ascii_toupper (name[0]);
  return name;
}

/* FALSE once the page was popped or the window went away: the page can
 * outlive both while an asynchronous operation is still running. */
static gboolean
page_is_shown (AmlInstancePage *self)
{
  return gtk_widget_get_root (GTK_WIDGET (self)) != NULL;
}

static void
show_toast (AmlInstancePage *self, const char *message)
{
  AdwToast *toast = NULL;

  if (self->toast_overlay == NULL || !page_is_shown (self))
    return;

  toast = adw_toast_new (message);
  /* Messages come from GError: they are plain text, never markup. */
  adw_toast_set_use_markup (toast, FALSE);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
}

/* The grid binds an item when it enters the store, so a card that was
 * already bound keeps what it shows: the instance has to leave the store
 * and come back at the same place for the card to be bound again. The
 * order of the cards does not change. */
static void
refresh_in_grid (AmlInstancePage *self)
{
  GListStore *store = aml_instance_manager_get_instances (self->manager);
  guint position = 0;

  if (!g_list_store_find (store, self->instance, &position))
    return;

  g_list_store_remove (store, position);
  g_list_store_insert (store, position, self->instance);
}

/* ---------------------------------------------------------- *
 * Saving the editable rows
 * ---------------------------------------------------------- */

static void
on_name_apply (AdwEntryRow *row, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (row));
  g_autofree char *current = instance_string (self->instance, "name");
  g_autoptr (GError) error = NULL;

  if (g_strcmp0 (text, current) == 0)
    return;

  if (!aml_instance_manager_rename (self->manager, self->instance, text,
                                    &error))
    {
      /* The edit never reached the file: the row shows the stored name
       * again instead of a name that does not exist. */
      show_toast (self, error->message);
      gtk_editable_set_text (GTK_EDITABLE (row), current);
      return;
    }

  g_autofree char *renamed = instance_string (self->instance, "name");

  gtk_editable_set_text (GTK_EDITABLE (row), renamed);
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self), renamed);
  refresh_in_grid (self);
}

static void
on_jvm_args_apply (AdwEntryRow *row, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (row));
  g_autofree char *current = instance_string (self->instance, "jvm-args");
  g_autoptr (GError) error = NULL;

  if (g_strcmp0 (text, current) == 0)
    return;

  /* AmlInstanceManager validates before it writes, so an empty line
   * fails here and never reaches instances.json. */
  g_object_set (self->instance, "jvm-args", text, NULL);

  if (!aml_instance_manager_save (self->manager, &error))
    {
      g_object_set (self->instance, "jvm-args", current, NULL);
      show_toast (self, error->message);
      gtk_editable_set_text (GTK_EDITABLE (row), current);
      return;
    }
}

/* Picking another version stores it the way the name row stores a
 * name. The row answers nothing while it is still loading the list or
 * after it failed, so there is no version to store then. */
static void
on_version_selected (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  AmlInstancePage *self = user_data;
  AmlVersionRow *row = AML_VERSION_ROW (object);
  const char *version = aml_version_row_get_selected (row);
  g_autofree char *current =
    instance_string (self->instance, "minecraft-version");
  g_autoptr (GError) error = NULL;

  if (version == NULL)
    return;

  if (g_strcmp0 (version, current) == 0)
    return;

  g_object_set (self->instance, "minecraft-version", version, NULL);

  if (!aml_instance_manager_save (self->manager, &error))
    {
      /* The edit never reached the file: the instance and the row go
       * back to the version that is still stored. */
      g_object_set (self->instance, "minecraft-version", current, NULL);
      show_toast (self, error->message);
      aml_version_row_select (row, current);
      return;
    }

  /* The card in the grid shows the version this instance runs. */
  refresh_in_grid (self);
}

/* ---------------------------------------------------------- *
 * The icon row
 * ---------------------------------------------------------- */

static void
update_icon_row (AmlInstancePage *self)
{
  g_autofree char *icon = instance_string (self->instance, "icon");

  if (self->icon_row == NULL || self->icon_image == NULL)
    return;

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->icon_row),
                               icon != NULL ? icon : "Default");

  if (icon != NULL && g_file_test (icon, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (GFile) file = g_file_new_for_path (icon);
      g_autoptr (GError) error = NULL;
      g_autoptr (GdkTexture) texture = gdk_texture_new_from_file (file, &error);

      if (texture != NULL)
        {
          gtk_image_set_from_paintable (GTK_IMAGE (self->icon_image),
                                        GDK_PAINTABLE (texture));
          return;
        }

      /* Not a readable image: fall back to the placeholder instead of
       * showing a broken picture. */
    }

  gtk_image_set_from_icon_name (GTK_IMAGE (self->icon_image),
                                "applications-games-symbolic");
}

static void
on_icon_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file == NULL)
    {
      /* Dismissing the chooser is not a failure. */
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED))
        show_toast (self, error->message);

      g_object_unref (self);
      return;
    }

  g_autofree char *path = g_file_get_path (file);

  if (path == NULL)
    {
      show_toast (self, "The icon must be a file on this computer");
      g_object_unref (self);
      return;
    }

  g_autofree char *previous = instance_string (self->instance, "icon");

  g_object_set (self->instance, "icon", path, NULL);

  if (!aml_instance_manager_save (self->manager, &error))
    {
      g_object_set (self->instance, "icon", previous, NULL);
      show_toast (self, error->message);
      g_object_unref (self);
      return;
    }

  update_icon_row (self);
  g_object_unref (self);
}

static void
on_change_icon_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  gtk_file_dialog_set_title (dialog, "Choose an Icon");
  gtk_file_filter_set_name (filter, "Images");
  gtk_file_filter_add_mime_type (filter, "image/*");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));

  gtk_file_dialog_open (dialog,
                        GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, on_icon_chosen, g_object_ref (self));
}

/* ---------------------------------------------------------- *
 * Open folder, duplicate, delete
 * ---------------------------------------------------------- */

static void
on_open_folder_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_instance_manager_open_folder_finish (AML_INSTANCE_MANAGER (source),
                                                result, &error))
    show_toast (self, error->message);

  g_object_unref (self);
}

static void
on_open_folder_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlInstancePage *self = user_data;

  aml_instance_manager_open_folder_async (self->manager, self->instance, NULL,
                                          on_open_folder_ready,
                                          g_object_ref (self));
}

static void
on_duplicate_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlInstance) copy =
    aml_instance_manager_duplicate_finish (AML_INSTANCE_MANAGER (source),
                                           result, &error);

  if (copy == NULL)
    show_toast (self, error->message);
  else
    {
      g_autofree char *name = instance_string (copy, "name");
      g_autofree char *message = g_strdup_printf ("Duplicated to \"%s\"", name);

      show_toast (self, message);
    }

  g_object_unref (self);
}

static void
on_duplicate_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlInstancePage *self = user_data;

  aml_instance_manager_duplicate_async (self->manager, self->instance, NULL,
                                        on_duplicate_ready,
                                        g_object_ref (self));
}

static void
on_delete_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_instance_manager_delete_finish (AML_INSTANCE_MANAGER (source),
                                           result, &error))
    {
      /* The instance is still registered: stay on its page. */
      show_toast (self, error->message);
      g_object_unref (self);
      return;
    }

  GtkWidget *view =
    gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_NAVIGATION_VIEW);

  /* Only go back if this page is the one on top; the user may have
   * left it while the directory was being removed. */
  if (view != NULL &&
      adw_navigation_view_get_visible_page (ADW_NAVIGATION_VIEW (view)) ==
      ADW_NAVIGATION_PAGE (self))
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (view));

  g_object_unref (self);
}

static void
on_delete_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "delete") == 0 && self->manager != NULL)
    aml_instance_manager_delete_async (self->manager, self->instance, TRUE,
                                       NULL, on_delete_ready,
                                       g_object_ref (self));

  g_object_unref (self);
}

static void
on_delete_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autofree char *name = instance_string (self->instance, "name");
  g_autofree char *heading = g_strdup_printf ("Delete \"%s\"?", name);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (heading,
                          "Its worlds, mods and settings are deleted "
                          "permanently. This cannot be undone."));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);

  adw_alert_dialog_add_response (alert, "cancel", "Cancel");
  adw_alert_dialog_add_response (alert, "delete", "Delete");
  adw_alert_dialog_set_response_appearance (alert, "delete",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_delete_chosen,
                           g_object_ref (self));
}

/* ---------------------------------------------------------- *
 * Rows
 * ---------------------------------------------------------- */

/* A row that only displays a value: nothing can activate it. */
static GtkWidget *
value_row (const char *title, const char *value)
{
  GtkWidget *row = adw_action_row_new ();

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);

  return row;
}

/* A row whose title, its whole area and its suffix button all do the
 * same thing. */
static GtkWidget *
button_row (AdwPreferencesGroup *group,
            const char          *title,
            const char          *subtitle,
            const char          *icon_name,
            GCallback            callback,
            gpointer             user_data)
{
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *button = gtk_button_new_from_icon_name (icon_name);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);

  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (button, title);
  g_signal_connect (button, "clicked", callback, user_data);

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), button);
  adw_preferences_group_add (group, row);

  return row;
}

/* ---------------------------------------------------------- *
 * The five pages
 * ---------------------------------------------------------- */

static GtkWidget *
create_settings_page (AmlInstancePage *self)
{
  GtkWidget *page = adw_preferences_page_new ();
  g_autofree char *name = instance_string (self->instance, "name");
  g_autofree char *directory = instance_string (self->instance, "directory");
  g_autofree char *minecraft_version =
    instance_string (self->instance, "minecraft-version");
  g_autofree char *loader = loader_display_name (self->instance);
  g_autofree char *loader_version =
    instance_string (self->instance, "loader-version");
  g_autofree char *jvm_args = instance_string (self->instance, "jvm-args");
  GtkWidget *name_row = adw_entry_row_new ();
  GtkWidget *version_row = GTK_WIDGET (aml_version_row_new ());
  GtkWidget *jvm_args_row = adw_entry_row_new ();
  GtkWidget *general = adw_preferences_group_new ();
  GtkWidget *minecraft = adw_preferences_group_new ();
  GtkWidget *java = adw_preferences_group_new ();
  GtkWidget *actions = adw_preferences_group_new ();
  GtkWidget *change_icon = gtk_button_new_with_label ("Change");
  GtkWidget *delete_row = NULL;

  gtk_widget_set_vexpand (page, TRUE);

  /* General */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (general), "General");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_row), "Name");
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (name_row), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (name_row), name);
  g_signal_connect (name_row, "apply", G_CALLBACK (on_name_apply), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (general), name_row);

  self->icon_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->icon_row), "Icon");
  self->icon_image = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (self->icon_image), 32);
  adw_action_row_add_prefix (ADW_ACTION_ROW (self->icon_row), self->icon_image);

  gtk_widget_set_valign (change_icon, GTK_ALIGN_CENTER);
  g_signal_connect (change_icon, "clicked",
                    G_CALLBACK (on_change_icon_clicked), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->icon_row), change_icon);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->icon_row),
                                         change_icon);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (general), self->icon_row);
  update_icon_row (self);

  button_row (ADW_PREFERENCES_GROUP (general), "Directory", directory,
              "folder-open-symbolic", G_CALLBACK (on_open_folder_clicked),
              self);

  /* Minecraft: the list of versions comes from Mojang's manifest,
   * fetched as the page is built. The version this instance runs stays
   * in the list even when the filter would hide its type. */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (minecraft),
                                   "Minecraft");
  g_signal_connect (version_row, "notify::selected",
                    G_CALLBACK (on_version_selected), self);
  aml_version_row_load (AML_VERSION_ROW (version_row), self->settings,
                        minecraft_version);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (minecraft), version_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (minecraft),
                             value_row ("Loader", loader));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (minecraft),
                             value_row ("Loader Version",
                                        loader_version != NULL
                                          ? loader_version
                                          : "None"));

  /* Java */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (java), "Java");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (java),
                             value_row ("Java Installation", "Default"));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (jvm_args_row),
                                 "JVM Arguments");
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (jvm_args_row), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (jvm_args_row), jvm_args);
  g_signal_connect (jvm_args_row, "apply", G_CALLBACK (on_jvm_args_apply),
                    self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (java), jvm_args_row);

  /* Actions */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (actions), "Actions");
  button_row (ADW_PREFERENCES_GROUP (actions), "Duplicate",
              "Create a copy of this instance", "edit-copy-symbolic",
              G_CALLBACK (on_duplicate_clicked), self);
  button_row (ADW_PREFERENCES_GROUP (actions), "Open Folder", NULL,
              "folder-open-symbolic", G_CALLBACK (on_open_folder_clicked),
              self);
  delete_row = button_row (ADW_PREFERENCES_GROUP (actions), "Delete Instance",
                           "Remove this instance from disk",
                           "user-trash-symbolic",
                           G_CALLBACK (on_delete_clicked), self);
  gtk_widget_add_css_class (
    adw_action_row_get_activatable_widget (ADW_ACTION_ROW (delete_row)),
    "destructive-action");

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (general));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (minecraft));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (java));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (actions));

  return page;
}

/* One honest placeholder: no rows, no buttons, nothing that looks
 * like it works. */
static GtkWidget *
create_placeholder_page (const char *title)
{
  GtkWidget *page = adw_status_page_new ();

  adw_status_page_set_title (ADW_STATUS_PAGE (page), title);
  adw_status_page_set_description (ADW_STATUS_PAGE (page),
                                   "This feature is not available yet.");

  return page;
}

AmlInstancePage *
aml_instance_page_new (AmlInstanceManager *manager, AmlInstance *instance,
                       AmlSettings *settings)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (manager), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (AML_IS_SETTINGS (settings), NULL);

  AmlInstancePage *self = g_object_new (AML_TYPE_INSTANCE_PAGE, NULL);
  GtkWidget *stack = adw_view_stack_new ();
  GtkWidget *switcher = adw_view_switcher_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *toolbar = adw_toolbar_view_new ();
  g_autofree char *name = instance_string (instance, "name");

  self->manager = g_object_ref (manager);
  self->instance = g_object_ref (instance);
  self->settings = g_object_ref (settings);

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack),
                                       create_settings_page (self),
                                       "settings", "Settings",
                                       "preferences-system-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack), create_placeholder_page ("Resource Packs"),
    "resource-packs", "Resource Packs", "image-x-generic-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack), create_placeholder_page ("Shader Packs"),
    "shader-packs", "Shader Packs", "applications-graphics-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack), create_placeholder_page ("Mods"),
    "mods", "Mods", "view-list-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack), create_placeholder_page ("Logs"),
    "logs", "Logs", "text-x-generic-symbolic");

  /* The header holds only this switcher: AdwHeaderBar adds the back
   * button and the window controls by itself. */
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher),
                                ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher),
                               ADW_VIEW_STACK (stack));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);

  self->toast_overlay = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toast_overlay), stack);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar),
                                self->toast_overlay);

  /* The title is for the screen reader and for the tooltip of a back
   * button pushed above this page; the header itself shows the view
   * switcher, never the instance name. */
  adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self), toolbar);
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self), name);

  return self;
}

static void
aml_instance_page_dispose (GObject *object)
{
  AmlInstancePage *self = AML_INSTANCE_PAGE (object);

  /* The widget tree may be freed while the page object is kept alive
   * by a running operation: never touch it afterwards. The manager and
   * the instance stay, so those operations can still finish. */
  self->toast_overlay = NULL;
  self->icon_row = NULL;
  self->icon_image = NULL;

  G_OBJECT_CLASS (aml_instance_page_parent_class)->dispose (object);
}

static void
aml_instance_page_finalize (GObject *object)
{
  AmlInstancePage *self = AML_INSTANCE_PAGE (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (aml_instance_page_parent_class)->finalize (object);
}

static void
aml_instance_page_class_init (AmlInstancePageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_instance_page_dispose;
  object_class->finalize = aml_instance_page_finalize;
}

static void
aml_instance_page_init (AmlInstancePage *self)
{
  /* Everything is built in aml_instance_page_new(), which knows the
   * instance the page is for. */
  (void) self;
}
