/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstancePage: the page of one instance.
 *
 * The settings page is where the instance itself is edited: every edit
 * is written through AmlInstanceManager, so what the page shows is what
 * instances.json holds. Resource Packs, Shader Packs and Mods are one
 * content page with a different content type, which reads the folder of
 * the instance and asks Modrinth only about what fits it. Logs shows
 * what the game of this instance printed, through the launcher the
 * window shares. Duplicating, opening the folder and deleting run as
 * asynchronous GIO operations and never block the main loop. */

#include "aml-instance-page.h"

#include "aml-content-page.h"
#include "aml-version-row.h"
#include "aml-java.h"
#include "aml-java-row.h"
#include "aml-launcher.h"
#include "aml-loader-row.h"
#include "aml-logs-page.h"
#include "aml-minecraft-versions.h"

struct _AmlInstancePage
{
  AdwNavigationPage parent_instance;

  AmlInstanceManager *manager;
  AmlInstance *instance;
  AmlSettings *settings;
  AmlLauncher *launcher;

  /* Borrowed from the widget tree of the page. Cleared in dispose(),
   * because the tree can go away while a running operation still keeps
   * this page alive. */
  GtkWidget *toast_overlay;
  GtkWidget *icon_row;
  GtkWidget *icon_image;

  /* The Java row plus what we know about it. Row load = detection
   * finishes; description load = the Minecraft version's javaVersion.  */
  GtkWidget *java_row;
  int required_java;        /* 0 means "never found one / unknown" */
  GCancellable *cancellable;

  /* The loader and the version of it, and what the instance runs now, so
     that a change that cannot be written goes back to it. */
  GtkWidget *loader_row;
  AmlLoader loader;
  char *loader_version;
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

static void
on_manifest_loaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data);

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

  /* A loader has one set of versions per Minecraft version, so the list
     is asked again for the version that was picked. */
  aml_loader_row_load (AML_LOADER_ROW (self->loader_row), self->settings,
                       aml_loader_row_get_loader (AML_LOADER_ROW (
                                                    self->loader_row)),
                       self->loader_version, version);

  /* Its javaVersion requirement may differ: check the compatibility
   * warning again, using the manifest cache. */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
  aml_version_manifest_fetch_async (AML_VERSION_MANIFEST_URL,
                                    self->cancellable, on_manifest_loaded,
                                    g_object_ref (self));
}

/* ---------------------------------------------------------- *
 * The loader and the version of it
 * ---------------------------------------------------------- */

/* The loader and its version are what the instance runs, so they are
   written through the manager the way the Minecraft version above is. A
   version the row answers nothing for is nothing to store. */
static void
on_loader_changed (AmlLoaderRow *row, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  AmlLoader loader = aml_loader_row_get_loader (row);
  const char *version = aml_loader_row_get_version (row);
  g_autofree char *minecraft =
    instance_string (self->instance, "minecraft-version");
  g_autoptr (GError) error = NULL;

  /* While the row is loading, or when the loader has no version for this
     Minecraft version, there is no choice to store. */
  if (loader != AML_LOADER_VANILLA && version == NULL)
    return;

  g_object_set (self->instance, "loader", loader, "loader-version",
                loader == AML_LOADER_VANILLA ? NULL : version, NULL);

  if (!aml_instance_manager_save (self->manager, &error))
    {
      /* The edit never reached the file: the instance and the row go back
         to the loader the instance runs. */
      g_object_set (self->instance, "loader", self->loader, "loader-version",
                    self->loader_version, NULL);
      show_toast (self, error->message);
      aml_loader_row_load (row, self->settings, self->loader,
                           self->loader_version, minecraft);
      return;
    }

  self->loader = loader;
  g_free (self->loader_version);
  self->loader_version = g_strdup (loader == AML_LOADER_VANILLA
                                     ? NULL
                                     : version);

  /* The card in the grid shows what the instance runs. */
  refresh_in_grid (self);
}

/* ---------------------------------------------------------- *
 * The Java row
 * ---------------------------------------------------------- */

static void
java_status_refresh (AmlInstancePage *self)
{
  AmlJavaRow *row = AML_JAVA_ROW (self->java_row);
  const AmlJava *effective = NULL;
  const char *global = NULL;
  const char *message = NULL;
  GPtrArray *detected;
  g_autofree char *current = NULL;

  if (self->java_row == NULL || !aml_java_row_get_ready (row))
    return;

  detected = aml_java_row_get_detected (row);
  current = instance_string (self->instance, "java-path");

  if (detected->len == 0 && current == NULL)
    message = "No Java installation was found on this system";

  if (message == NULL && self->required_java > 0)
    {
      if (current != NULL)
        effective = aml_java_select (detected, current, NULL,
                                     self->required_java);
      else
        {
          global = aml_settings_get_default_java_path (self->settings);
          effective = aml_java_select (detected, NULL, global,
                                       self->required_java);
        }

      if (effective == NULL && (current != NULL || global != NULL))
        message = "The configured Java was not detected; make sure it "
                  "is still a usable java executable";
      else if (effective == NULL)
        {
          g_autofree char *description = g_strdup_printf (
            "No detected Java satisfies the requirement of Java %d+",
            self->required_java);

          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), description);
          return;
        }
      else if (effective->major_version < self->required_java)
        {
          g_autofree char *description = g_strdup_printf (
            "This game needs Java %d or newer, selected Java is %d",
            self->required_java, effective->major_version);

          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), description);
          return;
        }
    }

  /* A usable JVM needs AWT's X11 bridge; warn for a headless runtime
   * even when nothing else is wrong with the selection. */
  if (message == NULL)
    {
      if (effective == NULL && current != NULL)
        effective = aml_java_select (detected, current, NULL, 0);
      else if (effective == NULL)
        effective = aml_java_select (detected, NULL,
                                     aml_settings_get_default_java_path (
                                       self->settings),
                                     0);

      if (effective != NULL && effective->headless)
        message = "Minecraft needs a non-headless Java: on Arch use the "
                  "jre-openjdk package, not jre-openjdk-headless";
    }

  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), message);
}

static void
on_description_loaded (GObject *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionDescription) description =
    aml_version_load_finish (result, &error);

  if (description != NULL && description->java_version != NULL)
    self->required_java = description->java_version->major_version;
  else
    self->required_java = 0;

  java_status_refresh (self);
  g_object_unref (self);
}

static void
on_manifest_loaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionManifest) manifest =
    aml_version_manifest_fetch_finish (result, &error);
  g_autofree char *version = instance_string (self->instance,
                                              "minecraft-version");

  g_clear_object (&self->cancellable);

  if (manifest == NULL)
    {
      java_status_refresh (self);
      g_object_unref (self);
      return;
    }

  self->cancellable = g_cancellable_new ();
  aml_version_load_async (manifest, version, self->cancellable,
                          on_description_loaded, g_object_ref (self));
  g_object_unref (self);
}

/* Picks the row of the Java entry the user has chosen and writes it to
 * the instance: NULL for "Default". On writing failure the row and the
 * instance go back to the stored path and the row says why. */
static void
on_java_selected (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                  gpointer user_data)
{
  AmlInstancePage *self = user_data;
  AmlJavaRow *row = AML_JAVA_ROW (object);
  const char *path = aml_java_row_get_selected_path (row);
  g_autofree char *current = instance_string (self->instance, "java-path");
  g_autoptr (GError) error = NULL;
  const char *next;

  if (path == NULL)
    return; /* still loading or failed: nothing valid to store */

  next = (path[0] != '\0') ? path : NULL;

  if (g_strcmp0 (current, next) == 0)
    {
      java_status_refresh (self);
      return;
    }

  g_object_set (self->instance, "java-path", next, NULL);

  if (!aml_instance_manager_save (self->manager, &error))
    {
      g_object_set (self->instance, "java-path", current, NULL);
      show_toast (self, error->message);
      aml_java_row_select_path (row, current);
      return;
    }

  java_status_refresh (self);
}

static void
custom_java_probed (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlJava) probe = aml_java_probe_finish (result, &error);

  if (probe == NULL)
    {
      show_toast (self, error != NULL
                        ? error->message
                        : "the chosen file does not report a Java version");
      g_object_unref (self);
      return;
    }

  /* The custom entry goes into the row; selecting it fires our
   * notify::selected handler so the instance is updated from there. */
  aml_java_row_set_custom (AML_JAVA_ROW (self->java_row), probe->executable,
                           g_steal_pointer (&probe));
  g_object_unref (self);
}

static void
on_custom_java_chosen (GObject *dialog, GAsyncResult *result,
                       gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_open_finish (GTK_FILE_DIALOG (dialog), result, &error);

  g_object_unref (dialog);

  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED))
        show_toast (self, error->message);

      g_object_unref (self);
      return;
    }

  g_autofree char *path = g_file_get_path (file);

  aml_java_probe_async (path, NULL, custom_java_probed,
                        g_object_ref (self));
  g_object_unref (self);
}

static void
on_custom_java_clicked (GtkButton *button G_GNUC_UNUSED,
                        gpointer user_data)
{
  AmlInstancePage *self = user_data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, "Choose Java executable");
  gtk_file_dialog_open (dialog,
                        GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, on_custom_java_chosen, g_object_ref (self));
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
  g_autofree char *java_path =
    instance_string (self->instance, "java-path");
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
  /* The loader and one of its versions: changing either of them changes
     what this instance runs, so both are stored like the version above. */
  {
    AmlLoader loader = AML_LOADER_VANILLA;

    g_object_get (self->instance, "loader", &loader, NULL);
    self->loader = loader;
    self->loader_version = g_steal_pointer (&loader_version);
  }

  self->loader_row = GTK_WIDGET (aml_loader_row_new ());
  g_signal_connect (self->loader_row, "changed",
                    G_CALLBACK (on_loader_changed), self);
  aml_loader_row_load (AML_LOADER_ROW (self->loader_row), self->settings,
                       self->loader, self->loader_version, minecraft_version);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (minecraft),
                             self->loader_row);

  /* Java */
  GtkWidget *java_row = GTK_WIDGET (aml_java_row_new ());
  GtkWidget *custom_java_row = adw_action_row_new ();
  GtkWidget *custom_java_button = gtk_button_new_with_label ("Choose…");

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (java), "Java");

  g_signal_connect (java_row, "notify::selected",
                    G_CALLBACK (on_java_selected), self);
  aml_java_row_set_default_entry_label (AML_JAVA_ROW (java_row), "Default");
  aml_java_row_load (AML_JAVA_ROW (java_row), java_path);
  self->java_row = java_row;

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (custom_java_row),
                                 "Custom Java…");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (custom_java_row),
                               "Pick a java executable AML does not detect");
  gtk_widget_set_valign (custom_java_button, GTK_ALIGN_CENTER);
  g_signal_connect (custom_java_button, "clicked",
                    G_CALLBACK (on_custom_java_clicked), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (custom_java_row),
                             custom_java_button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (custom_java_row),
                                         custom_java_button);

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (java), java_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (java),
                             custom_java_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (jvm_args_row),
                                 "JVM Arguments");
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (jvm_args_row), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (jvm_args_row), jvm_args);
  g_signal_connect (jvm_args_row, "apply", G_CALLBACK (on_jvm_args_apply),
                    self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (java), jvm_args_row);

  /* The Java row's compatibility warning needs the javaVersion field of
   * the instance's version document; fetching is offline-friendly via
   * the cache. */
  self->cancellable = g_cancellable_new ();
  aml_version_manifest_fetch_async (AML_VERSION_MANIFEST_URL,
                                    self->cancellable, on_manifest_loaded,
                                    g_object_ref (self));

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

AmlInstancePage *
aml_instance_page_new (AmlInstanceManager *manager, AmlInstance *instance,
                       AmlSettings *settings, AmlLauncher *launcher)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (manager), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (AML_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (AML_IS_LAUNCHER (launcher), NULL);

  AmlInstancePage *self = g_object_new (AML_TYPE_INSTANCE_PAGE, NULL);
  GtkWidget *stack = adw_view_stack_new ();
  GtkWidget *switcher = adw_view_switcher_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *toolbar = adw_toolbar_view_new ();
  g_autofree char *name = instance_string (instance, "name");

  self->manager = g_object_ref (manager);
  self->instance = g_object_ref (instance);
  self->settings = g_object_ref (settings);
  self->launcher = g_object_ref (launcher);

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack),
                                       create_settings_page (self),
                                       "settings", "Settings",
                                       "preferences-system-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (aml_content_page_new (instance, AML_CONTENT_RESOURCE_PACK)),
    "resource-packs", "Resource Packs", "image-x-generic-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (aml_content_page_new (instance, AML_CONTENT_SHADER_PACK)),
    "shader-packs", "Shader Packs", "applications-graphics-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (aml_content_page_new (instance, AML_CONTENT_MOD)),
    "mods", "Mods", "view-list-symbolic");

  /* The logs of the instance are the logs of the launcher the window
     shares with the card, so the game can be watched from here. */
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack), GTK_WIDGET (aml_logs_page_new (launcher)),
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
  self->java_row = NULL;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (aml_instance_page_parent_class)->dispose (object);
}

static void
aml_instance_page_finalize (GObject *object)
{
  AmlInstancePage *self = AML_INSTANCE_PAGE (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);
  g_clear_object (&self->launcher);
  g_free (self->loader_version);

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
