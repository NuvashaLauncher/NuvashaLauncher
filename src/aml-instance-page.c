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
 * dialog shares. Duplicating, opening the folder and deleting run as
 * asynchronous GIO operations and never block the main loop. */

#include "aml-instance-page.h"
#include "aml-i18n.h"

#include "aml-content-page.h"
#include "aml-downloads.h"
#include "aml-version-row.h"
#include "aml-java.h"
#include "aml-java-row.h"
#include "aml-launcher.h"
#include "aml-loader-row.h"
#include "aml-logs-page.h"
#include "aml-minecraft-versions.h"
#include "aml-modpack.h"
#include "aml-ui-feedback.h"
#include "aml-task.h"

struct _AmlInstancePage
{
  GtkBox parent_instance;

  AmlInstanceManager *manager;
  AmlInstance *instance;
  AmlSettings *settings;
  AmlLauncher *launcher;
  gboolean export_include_transient;
  AmlTaskScope *scope;
  guint export_idle;

  /* Borrowed from the widget tree of the page. Cleared in dispose(),
   * because the tree can go away while a running operation still keeps
   * this page alive. */
  AdwToastOverlay *toast_overlay;
  AdwActionRow *icon_row;
  GtkLabel *icon_path_label; /* the icon path inside the chooser button */
  AdwViewStack *stack; /* the view stack holding the pages of the instance */
  AmlLogsPage *logs_page; /* the last page, kept so a rebuild can move it back */

  /* The Directory row: the chooser button that says where the instance is
     and the spinner that shows a move in progress. A move in progress
     makes the button insensitive, so a second one cannot start on top of
     it. */
  AdwActionRow *directory_row;
  GtkButton *directory_change;
  GtkLabel *directory_path_label; /* the path inside the chooser button */
  AdwSpinner *directory_spinner;

  /* The Java row plus what we know about it. Row load = detection
   * finishes; description load = the Minecraft version's javaVersion.  */
  AmlJavaRow *java_row;
  int required_java;        /* 0 means "never found one / unknown" */
  GCancellable *cancellable;
  AmlDownload *export_download;
  char *export_name;
  char *export_summary;
  char *export_version;

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

/* "fabric" is shown as "Fabric", the same way the instance list writes it. */
static char *
loader_display_name (AmlInstance *instance)
{
  AmlLoader loader = AML_LOADER_VANILLA;
  const char *nick = NULL;

  g_object_get (instance, "loader", &loader, NULL);
  nick = aml_loader_to_string (loader);

  if (nick == NULL || nick[0] == '\0')
     return g_strdup (_("Unknown"));

  char *name = g_strdup (nick);
  name[0] = g_ascii_toupper (name[0]);
  return name;
}

static void
show_toast (AmlInstancePage *self, const char *message)
{
  aml_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toast_overlay), message);
}

/* The list binds an item when it enters the store, so a row that was
 * already bound keeps what it shows: the instance has to leave the store
 * and come back at the same place for the row to be bound again. The
 * order of the rows does not change. */
static void
refresh_in_list (AmlInstancePage *self)
{
  GListStore *store = aml_instance_manager_get_instances (self->manager);
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
static void
rebuild_content_pages (AmlInstancePage *self)
{
  static const struct {
    AmlContentType type;
    const char *name;
    const char *title;
    const char *icon;
  } pages[] = {
    { AML_CONTENT_RESOURCE_PACK, "resource-packs", "Resource Packs",
      "image-x-generic-symbolic" },
    { AML_CONTENT_SHADER_PACK, "shader-packs", "Shader Packs",
      "applications-graphics-symbolic" },
    { AML_CONTENT_MOD, "mods", "Mods", "view-list-symbolic" },
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
        GTK_WIDGET (aml_content_page_new (self->instance, pages[i].type)),
        pages[i].name, pages[i].title, pages[i].icon);
    }

  /* A page that is added again lands at the end, after Logs. Logs is put
     back where it belongs, last, so the order of the switcher never
     changes: Settings, Resource Packs, Shader Packs, Mods, Logs. */
  if (self->logs_page != NULL)
    {
      adw_view_stack_remove (ADW_VIEW_STACK (self->stack),
                             GTK_WIDGET (self->logs_page));
      adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack),
                                           GTK_WIDGET (self->logs_page), "logs", "Logs",
                                        "edit-clear-all-symbolic");
    }

  /* What the user was looking at is not thrown away by the rebuild. */
  if (visible != NULL)
    adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (self->stack),
                                           visible);
}

/* ---------------------------------------------------------- *
 * Saving the editable rows
 * ---------------------------------------------------------- */

static void
on_name_apply (AdwEntryRow *row, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (GTK_WIDGET (row)));
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
      gtk_editable_set_text (GTK_EDITABLE (GTK_WIDGET (row)), current);
      return;
    }

  g_autofree char *renamed = instance_string (self->instance, "name");

  gtk_editable_set_text (GTK_EDITABLE (GTK_WIDGET (row)), renamed);

  /* The tab follows the name: the window watches the instance for this,
     because the tab is the window's and not the page's. */
  refresh_in_list (self);
}

static void
on_jvm_args_apply (AdwEntryRow *row, gpointer user_data)
{
  AmlInstancePage *self = user_data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (GTK_WIDGET (row)));
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
      gtk_editable_set_text (GTK_EDITABLE (GTK_WIDGET (row)), current);
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

  /* The row shows the version this instance runs. */
  refresh_in_list (self);

  /* Resource Packs and Shader Packs are narrowed to the Minecraft
     version, so they are built again for the version that was picked. */
  rebuild_content_pages (self);

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

  /* The row shows what the instance runs. */
  refresh_in_list (self);

  /* A mod runs on a loader, and a resource pack or a shader pack is
     loaded by the game, so all three content pages depend on what was
     picked here and are built again. */
  rebuild_content_pages (self);
}

/* ---------------------------------------------------------- *
 * The Java row
 * ---------------------------------------------------------- */

static void
java_status_refresh (AmlInstancePage *self)
{
  AmlJavaRow *row = AML_JAVA_ROW (self->java_row);
  const AmlJava *effective = NULL;
  const char *message = NULL;
  GPtrArray *detected;
  g_autofree char *current = NULL;

  if (self->java_row == NULL || !aml_java_row_get_ready (row))
    return;

  detected = aml_java_row_get_detected (row);
  current = instance_string (self->instance, "java-path");

  /* The first entry names the runtime the automatic choice stands for,
     and that same runtime is taken out of the list below so it is not
     shown twice. */
  aml_java_row_set_required_major (row, self->required_java);

  if (detected->len == 0 && current == NULL)
    message = _("No Java installation was found on this system");

  if (message == NULL && self->required_java > 0)
    {
      effective = aml_java_select (detected, current, self->required_java);

      if (effective == NULL && current != NULL)
        message = _("The configured Java was not detected; make sure it "
                    "is still a usable java executable");
      else if (effective == NULL)
        {
          g_autofree char *description = g_strdup_printf (
            _("No detected Java satisfies the requirement of Java %d+"),
            self->required_java);

          adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)), description);
          return;
        }
      else if (effective->major_version < self->required_java)
        {
          g_autofree char *description = g_strdup_printf (
            _("This game needs Java %d or newer, selected Java is %d"),
            self->required_java, effective->major_version);

          adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)), description);
          return;
        }
    }

  /* A usable JVM needs AWT's X11 bridge; warn for a headless runtime
   * even when nothing else is wrong with the selection. */
  if (message == NULL)
    {
      if (effective == NULL)
        effective = aml_java_select (detected, current, 0);

      if (effective != NULL && effective->headless)
        message = _("Minecraft needs a non-headless Java: on Arch use the "
                    "jre-openjdk package, not jre-openjdk-headless");
    }

  adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)), message);
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
 * the instance: NULL for the automatic choice, which is what an
 * instance keeps unless the user names a runtime here. On writing
 * failure the row and the instance go back to the stored path and the
 * row says why. */
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

/* ---------------------------------------------------------- *
 * The icon row
 * ---------------------------------------------------------- */

/* A button that shows a folder, the path it is about and the arrow that
 * says pressing it chooses another one. The label is handed back so the
 * row can put the new path in it after the choice. */
static GtkWidget *
path_button (const char *path, GtkLabel **label)
{
  GtkButton *button = GTK_BUTTON (gtk_button_new ());
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkImage *image = GTK_IMAGE (gtk_image_new_from_icon_name ("folder-symbolic"));
  GtkLabel *text = GTK_LABEL (gtk_label_new (path));
  GtkImage *arrow = GTK_IMAGE (gtk_image_new_from_icon_name ("pan-end-symbolic"));

  /* A path is long, and the middle of it is what tells two paths apart.
     The cap keeps a deep path from making the row as wide as itself. */
  gtk_label_set_ellipsize (GTK_LABEL (GTK_WIDGET (text)), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (GTK_LABEL (GTK_WIDGET (text)), 30);

  gtk_widget_set_valign (GTK_WIDGET (image), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (text), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (arrow), GTK_ALIGN_CENTER);

  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (image));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (text));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (arrow));
  gtk_button_set_child (GTK_BUTTON (GTK_WIDGET (button)), GTK_WIDGET (box));

  if (label != NULL)
    *label = GTK_LABEL (text);

  return GTK_WIDGET (button);
}

static void
update_icon_row (AmlInstancePage *self)
{
  g_autofree char *icon = instance_string (self->instance, "icon");

  if (self->icon_path_label == NULL)
    return;

  gtk_label_set_text (GTK_LABEL (self->icon_path_label),
                      icon != NULL ? icon : "Default");
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
      show_toast (self, _("The icon must be a file on this computer"));
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

  gtk_file_dialog_set_title (dialog, _("Choose an Icon"));
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
on_export_progress (guint64 done, guint64 total, const char *detail,
                    gpointer user_data)
{
  AmlInstancePage *self = user_data;

  if (self->export_download != NULL)
    {
      aml_download_set_detail (self->export_download, detail);
      aml_download_set_progress (self->export_download, done, total);
    }
}

static void
on_export_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_modpack_export_finish (result, &error))
    show_toast (self, error->message);
  else
    show_toast (self, _("Modpack exported"));

  if (self->export_download != NULL)
    {
      aml_download_finish (self->export_download);
      self->export_download = NULL;
    }

  g_object_unref (self);
}

static void
on_export_file_chosen (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  AmlInstancePage *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

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

    if (path == NULL)
      show_toast (self, _("The export location is not a local file"));
    else
      {
        g_autofree char *name = instance_string (self->instance, "name");

        self->export_download =
          aml_downloads_add (AML_DOWNLOAD_UPDATE,
                             name != NULL ? name : _("Modpack export"));
        aml_download_set_detail (self->export_download,
                                 _("Exporting modpack"));
        g_object_add_weak_pointer (G_OBJECT (self->export_download),
                                   (gpointer *) &self->export_download);
        aml_modpack_export_async (self->instance, path, self->export_name,
                                  self->export_summary, self->export_version,
                                  self->export_include_transient,
                                  on_export_progress, self,
                                  aml_download_get_cancellable (
                                    self->export_download),
                                  on_export_done, g_object_ref (self));
      }
  }

  g_object_unref (self);
}

static void
open_export_file_dialog (AmlInstancePage *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_autofree char *name = instance_string (self->instance, "name");
  g_autofree char *filename = g_strdup_printf (
    "%s.mrpack", name != NULL ? name : "instance");

  gtk_file_dialog_set_title (dialog, _("Export Modpack"));
  gtk_file_dialog_set_initial_name (dialog, filename);
  gtk_file_filter_set_name (filter, _("Modrinth Modpacks (*.mrpack)"));
  gtk_file_filter_add_suffix (filter, "mrpack");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_save (dialog,
                        GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, on_export_file_chosen, g_object_ref (self));
}

static void show_export_policy (AmlInstancePage *self);

typedef struct {
  AmlInstancePage *page;
  AdwDialog *dialog;
  AdwEntryRow *name;
  AdwEntryRow *summary;
  AdwEntryRow *version;
} ExportMetadata;

static void
export_metadata_free (ExportMetadata *data)
{
  g_object_unref (data->page);
  g_object_unref (data->dialog);
  g_free (data);
}

static gboolean
export_metadata_free_idle (gpointer user_data)
{
  ExportMetadata *data = user_data;
  aml_task_scope_forget_source (data->page->scope, data->page->export_idle);
  data->page->export_idle = 0;
  return G_SOURCE_REMOVE;
}

static void
on_export_metadata_closed (AdwDialog *dialog G_GNUC_UNUSED,
                           gpointer user_data)
{
  /* Adwaita is still completing its close signal while this callback runs.
   * Release the dialog after the close sequence has finished. */
  ExportMetadata *data = user_data;
  data->page->export_idle = g_idle_add_full (
    G_PRIORITY_DEFAULT, export_metadata_free_idle, data,
    (GDestroyNotify) export_metadata_free);
  aml_task_scope_track_source (data->page->scope, data->page->export_idle);
}

static void
on_export_metadata_continue (GtkButton *button G_GNUC_UNUSED,
                             gpointer user_data)
{
  ExportMetadata *data = user_data;
  AmlInstancePage *self = data->page;

  g_free (self->export_name);
  g_free (self->export_summary);
  g_free (self->export_version);
  self->export_name = g_strdup (gtk_editable_get_text (
    GTK_EDITABLE (data->name)));
  self->export_summary = g_strdup (gtk_editable_get_text (
    GTK_EDITABLE (data->summary)));
  self->export_version = g_strdup (gtk_editable_get_text (
    GTK_EDITABLE (data->version)));

  adw_dialog_close (data->dialog);

  /* The transient-file choice remains a separate, deliberately simple
   * safety decision immediately before choosing the destination. */
  show_export_policy (self);
}

static void
on_export_metadata_cancel (GtkButton *button G_GNUC_UNUSED,
                           gpointer user_data)
{
  ExportMetadata *data = user_data;

  adw_dialog_close (data->dialog);
}

static void
show_export_metadata (AmlInstancePage *self)
{
  ExportMetadata *data = g_new0 (ExportMetadata, 1);
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkLabel *heading = GTK_LABEL (gtk_label_new (_("Pack information")));
  GtkLabel *description = GTK_LABEL (gtk_label_new (
    _("Optional metadata. Add a summary or version ID to help identify the "
      "exported pack.")));
  AdwPreferencesGroup *fields = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkButton *cancel = GTK_BUTTON (gtk_button_new_with_label (_("Cancel")));
  GtkButton *export_button = GTK_BUTTON (gtk_button_new_with_label (_("Export")));
  g_autofree char *name = instance_string (self->instance, "name");

  data->page = g_object_ref (self);
  data->dialog = ADW_DIALOG (g_object_ref_sink (adw_dialog_new ()));
  data->name = ADW_ENTRY_ROW (adw_entry_row_new ());
  data->summary = ADW_ENTRY_ROW (adw_entry_row_new ());
  data->version = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_dialog_set_title (data->dialog, _("Export modpack metadata"));
  g_signal_connect (data->dialog, "closed",
                    G_CALLBACK (on_export_metadata_closed), data);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (data->name),
                                  _("Pack Name"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (data->summary),
                                  _("Summary"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (data->version),
                                  _("Version ID"));
  gtk_widget_add_css_class (GTK_WIDGET (heading), "title-2");
  gtk_widget_add_css_class (GTK_WIDGET (description), "dim-label");
  gtk_label_set_justify (GTK_LABEL (GTK_WIDGET (heading)), GTK_JUSTIFY_CENTER);
  gtk_label_set_justify (GTK_LABEL (GTK_WIDGET (description)), GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap (GTK_LABEL (GTK_WIDGET (description)), TRUE);
  gtk_label_set_xalign (GTK_LABEL (GTK_WIDGET (heading)), 0.5f);
  gtk_label_set_xalign (GTK_LABEL (GTK_WIDGET (description)), 0.5f);
  gtk_editable_set_text (GTK_EDITABLE (data->name), name != NULL ? name : "");
  gtk_editable_set_text (GTK_EDITABLE (data->version), "");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (fields)),
                             GTK_WIDGET (data->name));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (fields)),
                             GTK_WIDGET (data->summary));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (fields)),
                             GTK_WIDGET (data->version));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (heading));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (description));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (fields));
  gtk_widget_set_margin_start (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (box), 12);
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (buttons));
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (cancel));
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (export_button));
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (export_button), "suggested-action");
  g_signal_connect (cancel, "clicked",
                    G_CALLBACK (on_export_metadata_cancel), data);
  g_signal_connect (export_button, "clicked",
                    G_CALLBACK (on_export_metadata_continue), data);
  adw_dialog_set_child (data->dialog, GTK_WIDGET (box));
  adw_dialog_present (data->dialog, GTK_WIDGET (self));
}

static void
on_export_policy_chosen (GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
  AmlInstancePage *self = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "cancel") != 0)
    {
      self->export_include_transient = g_strcmp0 (response, "include") == 0;
      open_export_file_dialog (self);
    }

  g_object_unref (self);
}

static void
on_export_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlInstancePage *self = user_data;

  if (aml_settings_get_export_metadata (self->settings))
    show_export_metadata (self);
  else
    {
      g_clear_pointer (&self->export_name, g_free);
      g_clear_pointer (&self->export_summary, g_free);
      g_clear_pointer (&self->export_version, g_free);
      show_export_policy (self);
    }
}

static void
show_export_policy (AmlInstancePage *self)
{
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (_("Include transient files?"),
                          _("Logs, crash reports, screenshots, caches and "
                            "downloads are normally excluded.")));

  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "clean", _("Clean Export"));
  adw_alert_dialog_add_response (alert, "include", _("Include All Files"));
  adw_alert_dialog_set_response_appearance (alert, "include",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "clean");
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_export_policy_chosen, g_object_ref (self));
}

/* ---------------------------------------------------------- *
 * Changing the directory
 * ---------------------------------------------------------- */

/* The row says where the instance is, and whether it may be moved right
 * now. A running game has its files open where they are, so the button
 * does nothing until it has stopped. */
static void
update_directory_row (AmlInstancePage *self)
{
  gboolean running =
    aml_launch_state_is_busy (aml_launcher_get_state (self->launcher));

  if (self->directory_row == NULL)
    return;

  if (self->directory_path_label != NULL)
    {
      g_autofree char *directory =
        instance_string (self->instance, "directory");

      gtk_label_set_text (GTK_LABEL (self->directory_path_label), directory);
    }

  gtk_widget_set_sensitive (GTK_WIDGET (self->directory_change), !running);
}

static void
set_moving (AmlInstancePage *self, gboolean moving)
{
  gtk_widget_set_visible (GTK_WIDGET (self->directory_spinner), moving);
  gtk_widget_set_sensitive (GTK_WIDGET (self->directory_change), !moving);
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
      show_toast (self, _("The game is running; stop it before moving its "
                          "directory"));
      return;
    }

  gtk_file_dialog_set_title (dialog, _("Choose a Directory"));
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
      g_autofree char *message =
        g_strdup_printf (_("Duplicated to \"%s\""), name);

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
  g_autofree char *heading = g_strdup_printf (_("Delete \"%s\"?"), name);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (heading,
                          "Its worlds, mods and settings are deleted "
                          "permanently. This cannot be undone."));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);

  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "delete", _("Delete"));
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
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkButton *button = GTK_BUTTON (gtk_button_new_from_icon_name (icon_name));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), title);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)), subtitle);

  gtk_widget_add_css_class (GTK_WIDGET (button), "flat");
  gtk_widget_set_valign (GTK_WIDGET (button), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (GTK_WIDGET (button), title);

  /* The row carries the title for a reader; the button is only its icon,
     so it needs a name of its own for a screen reader to say. */
  gtk_accessible_update_property (GTK_ACCESSIBLE (GTK_WIDGET (button)),
                                GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);

  g_signal_connect (button, "clicked", callback, user_data);

  adw_action_row_add_suffix (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (button));
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (button));
  adw_preferences_group_add (group, GTK_WIDGET (row));

  return GTK_WIDGET (row);
}

/* ---------------------------------------------------------- *
 * The five pages
 * ---------------------------------------------------------- */

static GtkWidget *
create_settings_page (AmlInstancePage *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
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
  AdwEntryRow *name_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  GtkWidget *version_row = GTK_WIDGET (aml_version_row_new ());
  AdwEntryRow *jvm_args_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  AdwPreferencesGroup *general = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *minecraft = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *java = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *actions = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkWidget *change_icon = NULL;
  GtkWidget *delete_row = NULL;

  /* Names, paths and arguments are text from the user or the disk: none
     of them is markup. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (name_row)), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (jvm_args_row)),
                                      FALSE);

  gtk_widget_set_vexpand (GTK_WIDGET (page), TRUE);

  /* General */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (GTK_WIDGET (general)), _("General"));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (name_row)), _("Name"));
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (GTK_WIDGET (name_row)), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (GTK_WIDGET (name_row)), name);
  g_signal_connect (name_row, "apply", G_CALLBACK (on_name_apply), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (general)), GTK_WIDGET (name_row));

  self->icon_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->icon_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->icon_row), _("Icon"));

  /* The button is the path and the chooser at once: pressing it picks a
     file, and what it picked is written in it. */
  change_icon = path_button (NULL, &self->icon_path_label);
  gtk_widget_add_css_class (change_icon, "flat");
  gtk_widget_set_valign (change_icon, GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text (change_icon, _("Choose an Icon"));
  g_signal_connect (change_icon, "clicked",
                    G_CALLBACK (on_change_icon_clicked), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->icon_row), change_icon);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->icon_row),
                                         change_icon);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (general)), GTK_WIDGET (self->icon_row));
  update_icon_row (self);

  /* Directory: where the files are. Pressing the button moves every file
     to another folder, which is the only thing the row does. */
  self->directory_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->directory_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->directory_row),
                                 _("Directory"));

   self->directory_change = GTK_BUTTON (path_button (
     directory, &self->directory_path_label));
  gtk_widget_add_css_class (GTK_WIDGET (self->directory_change), "flat");
  gtk_widget_set_valign (GTK_WIDGET (self->directory_change), GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text (GTK_WIDGET (self->directory_change), _("Change the Directory"));
  g_signal_connect (self->directory_change, "clicked",
                    G_CALLBACK (on_change_directory_clicked), self);

  self->directory_spinner = ADW_SPINNER (adw_spinner_new ());
  gtk_widget_set_valign (GTK_WIDGET (self->directory_spinner), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->directory_spinner), FALSE);

  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             GTK_WIDGET (self->directory_spinner));
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             GTK_WIDGET (self->directory_change));
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->directory_row),
                                         GTK_WIDGET (self->directory_change));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (general)),
                             GTK_WIDGET (self->directory_row));
  update_directory_row (self);

  /* Minecraft: the list of versions comes from Mojang's manifest,
   * fetched as the page is built. The version this instance runs stays
   * in the list even when the filter would hide its type. */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (GTK_WIDGET (minecraft)),
                                    _("Minecraft"));
  g_signal_connect (version_row, "notify::selected",
                    G_CALLBACK (on_version_selected), self);
  aml_version_row_load (AML_VERSION_ROW (version_row), self->settings,
                        minecraft_version);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (minecraft)), version_row);
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
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (minecraft)),
                             aml_loader_row_get_loader_row (
                               self->loader_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (minecraft)),
                             aml_loader_row_get_version_row (
                               self->loader_row));

  /* Java: the installations detection found. The row ends in an entry
     that adds one it did not report, so that is where they are added
     from. */
  GtkWidget *java_row = GTK_WIDGET (aml_java_row_new ());

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (GTK_WIDGET (java)), _("Java"));

  g_signal_connect (java_row, "notify::selected",
                    G_CALLBACK (on_java_selected), self);
  /* Known before the scan lands, so the first fill already names the
     runtime the automatic choice stands for and leaves it out of the
     list. The required version is not known yet and is set once the
     manifest loads. */
  aml_java_row_set_required_major (AML_JAVA_ROW (java_row),
                                   self->required_java);
  aml_java_row_load (AML_JAVA_ROW (java_row), java_path);
  self->java_row = AML_JAVA_ROW (java_row);

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (java)), java_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (jvm_args_row)),
                                 _("JVM Arguments"));
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (GTK_WIDGET (jvm_args_row)), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (GTK_WIDGET (jvm_args_row)), jvm_args);
  g_signal_connect (jvm_args_row, "apply", G_CALLBACK (on_jvm_args_apply),
                    self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (java)), GTK_WIDGET (jvm_args_row));

  /* The Java row's compatibility warning needs the javaVersion field of
   * the instance's version document; fetching is offline-friendly via
   * the cache. */
  self->cancellable = g_cancellable_new ();
  aml_version_manifest_fetch_async (AML_VERSION_MANIFEST_URL,
                                    self->cancellable, on_manifest_loaded,
                                    g_object_ref (self));

  /* Actions */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (GTK_WIDGET (actions)), _("Actions"));
  button_row (ADW_PREFERENCES_GROUP (actions), _("Duplicate"), NULL,
              "edit-copy-symbolic", G_CALLBACK (on_duplicate_clicked), self);
  button_row (ADW_PREFERENCES_GROUP (actions), _("Open Folder"), NULL,
              "folder-symbolic", G_CALLBACK (on_open_folder_clicked),
              self);
  button_row (ADW_PREFERENCES_GROUP (actions), _("Export Modpack"),
              NULL,
              "document-save-symbolic", G_CALLBACK (on_export_clicked), self);
  delete_row = button_row (ADW_PREFERENCES_GROUP (actions), _("Delete Instance"),
                           NULL, "user-trash-symbolic",
                           G_CALLBACK (on_delete_clicked), self);
  gtk_widget_add_css_class (
    adw_action_row_get_activatable_widget (ADW_ACTION_ROW (delete_row)),
    "destructive-action");

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (GTK_WIDGET (page)),
                            ADW_PREFERENCES_GROUP (general));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (GTK_WIDGET (page)),
                            ADW_PREFERENCES_GROUP (minecraft));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (GTK_WIDGET (page)),
                            ADW_PREFERENCES_GROUP (java));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (GTK_WIDGET (page)),
                            ADW_PREFERENCES_GROUP (actions));

  return GTK_WIDGET (page);
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
  AdwViewStack *stack = ADW_VIEW_STACK (adw_view_stack_new ());
  AdwViewSwitcher *switcher = ADW_VIEW_SWITCHER (adw_view_switcher_new ());
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  g_autofree char *name = instance_string (instance, "name");

  self->manager = g_object_ref (manager);
  self->instance = g_object_ref (instance);
  self->settings = g_object_ref (settings);
  self->launcher = g_object_ref (launcher);

  /* The Directory row cannot move files out from under a running game, so
     it follows the state of the launch the page shares with the row. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_page_launcher_state), self, 0);

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (GTK_WIDGET (stack)),
                                       create_settings_page (self),
                                        "settings", _("Settings"),
                                       "preferences-system-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (aml_content_page_new (instance, AML_CONTENT_RESOURCE_PACK)),
     "resource-packs", _("Resource Packs"), "image-x-generic-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (aml_content_page_new (instance, AML_CONTENT_SHADER_PACK)),
     "shader-packs", _("Shader Packs"), "applications-graphics-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (aml_content_page_new (instance, AML_CONTENT_MOD)),
     "mods", _("Mods"), "view-list-symbolic");

  /* The logs of the instance are the logs of the launcher the window
     shares with the row, so the game can be watched from here. */
  self->logs_page = aml_logs_page_new (launcher);
  g_object_ref (self->logs_page);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (GTK_WIDGET (stack)),
                                        GTK_WIDGET (self->logs_page), "logs", _("Logs"),
                                       "utilities-terminal-symbolic");

  /* Kept so that the content pages can be built again when the loader or
     the Minecraft version changes. */
  self->stack = ADW_VIEW_STACK (stack);

  /* The header holds only this switcher. There is no back button here:
   the dialog is named after the instance and its close button is what
   leaves it. */
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (GTK_WIDGET (switcher)),
                                ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (GTK_WIDGET (switcher)),
                               ADW_VIEW_STACK (stack));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (GTK_WIDGET (header)), GTK_WIDGET (switcher));

  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toast_overlay), GTK_WIDGET (stack));

  gtk_widget_set_vexpand (GTK_WIDGET (toolbar), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (toolbar), TRUE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)), GTK_WIDGET (header));
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)),
                                GTK_WIDGET (self->toast_overlay));

  /* The dialog holds this box, and the box holds the toolbar view of the
     instance: there is no navigation page and no back button between the
     switcher and the five pages of the instance. */
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (toolbar));
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
  self->icon_path_label = NULL;
  self->stack = NULL;
  g_clear_object (&self->logs_page);
  self->java_row = NULL;
  self->directory_row = NULL;
  self->directory_change = NULL;
  self->directory_path_label = NULL;
  self->directory_spinner = NULL;

  g_cancellable_cancel (self->cancellable);
  aml_task_scope_cancel (self->scope);
  g_clear_object (&self->cancellable);
  aml_task_scope_free (self->scope);
  self->scope = NULL;

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
  g_free (self->export_name);
  g_free (self->export_summary);
  g_free (self->export_version);

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
  self->scope = aml_task_scope_new (NULL, NULL, NULL, NULL);
}
