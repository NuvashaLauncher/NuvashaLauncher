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
  GtkBox parent_instance;

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

  /* The Directory row: the path, the two buttons that act on it and the
     spinner that shows a move in progress. A move in progress makes them
     insensitive, so a second one cannot start on top of it. */
  GtkWidget *directory_row;
  GtkWidget *directory_open;
  GtkWidget *directory_change;
  GtkWidget *directory_spinner;

  /* The Java row plus what we know about it. Row load = detection
   * finishes; description load = the Minecraft version's javaVersion.  */
  GtkWidget *java_row;
  int required_java;        /* 0 means "never found one / unknown" */
  GCancellable *cancellable;

  /* The two loader rows, plain AdwComboRows added to the Minecraft group,
     and what the instance runs now, so that a change that cannot be
     written goes back to it. */
  AmlLoaderRow *loader_row;
  AmlLoader loader;
  char *loader_version;
};

G_DEFINE_FINAL_TYPE (AmlInstancePage, aml_instance_page,
                     GTK_TYPE_BOX)

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

  /* The tab follows the name: the window watches the instance for this,
     because the tab is the window's and not the page's. */
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
  aml_loader_row_load (self->loader_row, self->settings,
                       aml_loader_row_get_loader (self->loader_row),
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

  /* A running game has the loader it started with; the change is not
     written down under it. */
  if (aml_launch_state_is_busy (aml_launcher_get_state (self->launcher)))
    return;

  /* While the row is loading, or when the loader has no version for this
     Minecraft version, there is no choice to store. */
  if (loader != AML_LOADER_VANILLA && version == NULL)
    return;

  /* A load that settled on the version the instance already runs is not
     a change. */
  if (loader == self->loader &&
      g_strcmp0 (loader == AML_LOADER_VANILLA ? NULL : version,
                 self->loader_version) == 0)
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

/* ---------------------------------------------------------- *
 * Changing the directory
 * ---------------------------------------------------------- */

/* The row says where the instance is, and which of the two things may be
 * done to it right now. A running game has its files open where they are,
 * so neither button does anything until it has stopped. */
static void
update_directory_row (AmlInstancePage *self)
{
  gboolean running =
    aml_launch_state_is_busy (aml_launcher_get_state (self->launcher));

  if (self->directory_row == NULL)
    return;

  adw_action_row_set_subtitle (
    ADW_ACTION_ROW (self->directory_row),
    instance_string (self->instance, "directory"));

  gtk_widget_set_sensitive (self->directory_open, !running);
  gtk_widget_set_sensitive (self->directory_change, !running);
}

static void
set_moving (AmlInstancePage *self, gboolean moving)
{
  gtk_widget_set_visible (self->directory_spinner, moving);
  gtk_widget_set_sensitive (self->directory_open, !moving);
  gtk_widget_set_sensitive (self->directory_change, !moving);
}

static void
on_change_directory_ready (GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;

  if (aml_instance_manager_change_directory_finish (
        AML_INSTANCE_MANAGER (source), result, &error))
    {
      /* The instance stores the new path; the row reads it from there, so
         what it says is what the file says. */
      update_directory_row (self);
    }
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      show_toast (self, error->message);
    }

  set_moving (self, FALSE);
  update_directory_row (self);

  g_object_unref (self);
}

static void
on_directory_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), result,
                                          &error);

  /* The chooser being dismissed is not a failure and there is nothing to
     report for it. */
  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED))
        show_toast (self, error->message);

      g_object_unref (self);
      return;
    }

  {
    g_autofree char *path = g_file_get_path (file);
    g_autofree char *canonical = NULL;

    /* Said before anything is touched, so a path that cannot work is
       refused with its reason and the instance stays as it was. */
    if (!aml_instance_manager_check_new_directory (self->manager,
                                                   self->instance, path,
                                                   &canonical, &error))
      {
        show_toast (self, error->message);
        g_object_unref (self);
        return;
      }

    set_moving (self, TRUE);

    aml_instance_manager_change_directory_async (
      self->manager, self->instance, canonical, NULL,
      on_change_directory_ready, g_object_ref (self));
  }

  g_object_unref (self);
}

static void
on_change_directory_clicked (GtkButton *button G_GNUC_UNUSED,
                             gpointer user_data)
{
  AmlInstancePage *self = user_data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();

  /* A game that is running has its files where the instance says they
     are; moving them out from under it would break the game and the move
     at the same time. */
  if (aml_launch_state_is_busy (aml_launcher_get_state (self->launcher)))
    {
      show_toast (self, "The game is running; stop it before moving its "
                   "directory");
      return;
    }

  gtk_file_dialog_set_title (dialog, "Choose a Directory");
  gtk_file_dialog_select_folder (dialog,
                                 GTK_IS_WINDOW (root) ? GTK_WINDOW (root)
                                                      : NULL,
                                 NULL, on_directory_chosen,
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

  /* Nothing more to do here: the instance left the list, and the window
     closes the tab of an instance that is no longer in it. Asking this
     page to close itself would mean two places deciding the same thing. */

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

  /* The row carries the title for a reader; the button is only its icon,
     so it needs a name of its own for a screen reader to say. */
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);

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

  /* Names, paths and arguments are text from the user or the disk: none
     of them is markup. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (name_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (jvm_args_row),
                                      FALSE);

  gtk_widget_set_vexpand (page, TRUE);

  /* General */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (general), "General");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_row), "Name");
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (name_row), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (name_row), name);
  g_signal_connect (name_row, "apply", G_CALLBACK (on_name_apply), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (general), name_row);

  self->icon_row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->icon_row),
                                      FALSE);
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

  /* Directory: where the files are, and the two things that can be done
     to them. Opening and changing are separate on purpose: one only
     looks, the other moves every file. */
  self->directory_row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->directory_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->directory_row),
                                 "Directory");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->directory_row),
                               directory);

  self->directory_open = gtk_button_new_with_label ("Open Folder");
  gtk_widget_add_css_class (self->directory_open, "flat");
  gtk_widget_set_valign (self->directory_open, GTK_ALIGN_CENTER);
  g_signal_connect (self->directory_open, "clicked",
                    G_CALLBACK (on_open_folder_clicked), self);

  self->directory_change = gtk_button_new_with_label ("Change");
  gtk_widget_add_css_class (self->directory_change, "flat");
  gtk_widget_set_valign (self->directory_change, GTK_ALIGN_CENTER);
  g_signal_connect (self->directory_change, "clicked",
                    G_CALLBACK (on_change_directory_clicked), self);

  self->directory_spinner = gtk_spinner_new ();
  gtk_widget_set_valign (self->directory_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->directory_spinner, FALSE);

  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             self->directory_spinner);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             self->directory_open);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             self->directory_change);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (general),
                             self->directory_row);
  update_directory_row (self);

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

  self->loader_row = aml_loader_row_new ();
  g_signal_connect (self->loader_row, "changed",
                    G_CALLBACK (on_loader_changed), self);
  aml_loader_row_load (self->loader_row, self->settings,
                       self->loader, self->loader_version, minecraft_version);

  /* Rows of the same Minecraft group, drawn and behaving like the version
     row above. */
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (minecraft),
                             aml_loader_row_get_loader_row (
                               self->loader_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (minecraft),
                             aml_loader_row_get_version_row (
                               self->loader_row));

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

  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (custom_java_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (custom_java_row),
                                 "Custom Java…");
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
  button_row (ADW_PREFERENCES_GROUP (actions), "Duplicate", NULL,
              "edit-copy-symbolic", G_CALLBACK (on_duplicate_clicked), self);
  button_row (ADW_PREFERENCES_GROUP (actions), "Open Folder", NULL,
              "folder-open-symbolic", G_CALLBACK (on_open_folder_clicked),
              self);
  delete_row = button_row (ADW_PREFERENCES_GROUP (actions), "Delete Instance",
                           NULL, "user-trash-symbolic",
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

/* The Directory row follows the launch: a running game makes both of its
   buttons insensitive, and stopping it makes them answer again. The
   loader cannot be changed under a running game either, so both of its
   rows follow the same state. */
static void
on_page_launcher_state (AmlLauncher *launcher G_GNUC_UNUSED,
                        AmlLaunchState state, const char *detail G_GNUC_UNUSED,
                        gpointer user_data)
{
  AmlInstancePage *self = user_data;

  update_directory_row (self);

  if (self->loader_row != NULL)
    aml_loader_row_set_blocked (
      self->loader_row,
      aml_launch_state_is_busy (state));
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

  /* The Directory row cannot move files out from under a running game, so
     it follows the state of the launch the page shares with the card. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_page_launcher_state), self, 0);

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

  /* The header holds only this switcher. There is no back button here:
   the tab bar of the window is what says which instance is open, and
   closing that tab is what leaves it. */
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher),
                                ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher),
                               ADW_VIEW_STACK (stack));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);

  self->toast_overlay = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toast_overlay), stack);

  gtk_widget_set_vexpand (toolbar, TRUE);
  gtk_widget_set_hexpand (toolbar, TRUE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar),
                                self->toast_overlay);

  /* The tab holds this box, and the box holds the toolbar view of the
     instance: there is no navigation page and no back button between the
     tab bar and the five pages of the instance. */
  gtk_box_append (GTK_BOX (self), toolbar);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);

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
  self->directory_row = NULL;
  self->directory_open = NULL;
  self->directory_change = NULL;
  self->directory_spinner = NULL;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  /* The loader rows are the page's. Their handlers go with them, so the
   * rows the widget tree may still hold cannot call back into a page
   * that is on its way out. */
  g_clear_object (&self->loader_row);

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
