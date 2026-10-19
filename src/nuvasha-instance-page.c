/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaInstancePage: settings, content, and logs for one instance.
 *
 * The settings page is where the instance itself is edited: every edit
 * Settings are persisted through NuvashaInstanceManager. Content pages share
 * one implementation with different content types. Logs shows this instance's
 * game output. File operations use asynchronous GIO calls. */

#include "nuvasha-instance-page-private.h"
#include "nuvasha-i18n.h"

#include "nuvasha-content-page.h"
#include "nuvasha-downloads.h"
#include "nuvasha-version-row.h"
#include "nuvasha-java.h"
#include "nuvasha-java-row.h"
#include "nuvasha-launcher.h"
#include "nuvasha-loader-row.h"
#include "nuvasha-logs-page.h"
#include "nuvasha-minecraft-versions.h"
#include "nuvasha-modpack.h"
#include "nuvasha-ui-feedback.h"
#include "nuvasha-task.h"
#include "nuvasha-operation.h"


G_DEFINE_FINAL_TYPE (NuvashaInstancePage, nuvasha_instance_page,
                     GTK_TYPE_BOX)

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

char *
instance_string (NuvashaInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

/* "fabric" is shown as "Fabric", the same way the instance list writes it. */
static char *
loader_display_name (NuvashaInstance *instance)
{
  NuvashaLoader loader = nuvasha_LOADER_VANILLA;
  const char *nick = NULL;

  g_object_get (instance, "loader", &loader, NULL);
  nick = nuvasha_loader_to_string (loader);

  if (nick == NULL || nick[0] == '\0')
     return g_strdup (_("Unknown"));

  char *name = g_strdup (nick);
  name[0] = g_ascii_toupper (name[0]);
  return name;
}

void
instance_page_show_toast (NuvashaInstancePage *self, const char *message)
{
  nuvasha_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toast_overlay), message);
}

/* The list binds an item when it enters the store, so a row that was
 * already bound keeps what it shows: the instance has to leave the store
 * and come back at the same place for the row to be bound again. The
 * order of the rows does not change. */
static void
refresh_in_list (NuvashaInstancePage *self)
{
  GListStore *store = nuvasha_instance_manager_get_instances (self->manager);
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
rebuild_content_pages (NuvashaInstancePage *self)
{
  static const struct {
    NuvashaContentType type;
    const char *name;
    const char *title;
    const char *icon;
  } pages[] = {
    { nuvasha_CONTENT_RESOURCE_PACK, "resource-packs", "Resource Packs",
      "image-x-generic-symbolic" },
    { nuvasha_CONTENT_SHADER_PACK, "shader-packs", "Shader Packs",
      "applications-graphics-symbolic" },
    { nuvasha_CONTENT_MOD, "mods", "Mods", "view-list-symbolic" },
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
        GTK_WIDGET (nuvasha_content_page_new (self->instance, pages[i].type)),
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

static void
on_content_tab_switched (GObject *stack, GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer user_data)
{
  (void) user_data;
  const char *name = adw_view_stack_get_visible_child_name (
    ADW_VIEW_STACK (stack));
  GtkWidget *child = adw_view_stack_get_child_by_name (
    ADW_VIEW_STACK (stack), name);

  if (child != NULL && nuvasha_IS_CONTENT_PAGE (child))
    nuvasha_content_page_reset_for_tab_switch (nuvasha_CONTENT_PAGE (child));
}

/* ---------------------------------------------------------- *
 * Saving the editable rows
 * ---------------------------------------------------------- */

static void
on_name_apply (AdwEntryRow *row, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (GTK_WIDGET (row)));
  g_autofree char *current = instance_string (self->instance, "name");
  g_autoptr (GError) error = NULL;

  if (g_strcmp0 (text, current) == 0)
    return;

  if (!nuvasha_instance_manager_rename (self->manager, self->instance, text,
                                    &error))
    {
      /* The edit never reached the file: the row shows the stored name
       * again instead of a name that does not exist. */
      instance_page_show_toast (self, error->message);
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
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (GTK_WIDGET (row)));
  g_autofree char *current = instance_string (self->instance, "jvm-args");
  g_autoptr (GError) error = NULL;

  if (g_strcmp0 (text, current) == 0)
    return;

  /* NuvashaInstanceManager validates before it writes, so an empty line
   * fails here and never reaches instances.json. */
  if (!nuvasha_instance_manager_set_string (self->manager, self->instance,
                                        "jvm-args", text, &error))
    {
      nuvasha_instance_set_string (self->instance, "jvm-args", current, NULL);
      instance_page_show_toast (self, error->message);
      gtk_editable_set_text (GTK_EDITABLE (GTK_WIDGET (row)), current);
      return;
    }
}

typedef struct
{
  NuvashaInstancePage *page;
  guint generation;
} VersionWait;

static void
version_wait_free (VersionWait *wait)
{
  g_object_unref (wait->page);
  g_free (wait);
}

static VersionWait *
version_wait_new (NuvashaInstancePage *self, guint generation)
{
  VersionWait *wait = g_new (VersionWait, 1);
  wait->generation = generation;
  wait->page = g_object_ref (self);
  return wait;
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
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  NuvashaVersionRow *row = nuvasha_VERSION_ROW (object);
  const char *version = nuvasha_version_row_get_selected (row);
  g_autofree char *current =
    instance_string (self->instance, "minecraft-version");
  g_autoptr (GError) error = NULL;

  if (version == NULL)
    return;

  if (g_strcmp0 (version, current) == 0)
    return;

  if (!nuvasha_instance_manager_set_string (self->manager, self->instance,
                                        "minecraft-version", version,
                                        &error))
    {
      /* The edit never reached the file: the instance and the row go
       * back to the version that is still stored. */
      nuvasha_instance_set_string (self->instance, "minecraft-version", current,
                               NULL);
      instance_page_show_toast (self, error->message);
      nuvasha_version_row_select (row, current);
      return;
    }

  /* The row shows the version this instance runs. */
  refresh_in_list (self);

  /* Resource Packs and Shader Packs are narrowed to the Minecraft
     version, so they are built again for the version that was picked. */
  rebuild_content_pages (self);

  /* A loader has one set of versions per Minecraft version, so the list
     is asked again for the version that was picked. */
  nuvasha_loader_row_load (self->loader_row, self->settings,
                       nuvasha_loader_row_get_loader (self->loader_row),
                       self->loader_version, version);

  /* Its javaVersion requirement may differ: check the compatibility
   * warning again, using the manifest cache. */
  if (self->versions_operation != NULL)
    {
      nuvasha_operation_cancel (self->versions_operation);
      nuvasha_operation_unref (self->versions_operation);
    }
  self->versions_operation = nuvasha_version_manifest_fetch_async (
    nuvasha_VERSION_MANIFEST_URL, on_manifest_loaded,
    version_wait_new (self, ++self->versions_generation));
}

/* ---------------------------------------------------------- *
 * The loader and the version of it
 * ---------------------------------------------------------- */

/* The loader and its version are what the instance runs, so they are
   written through the manager the way the Minecraft version above is. A
   version the row answers nothing for is nothing to store. */
static void
on_loader_changed (NuvashaLoaderRow *row, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  NuvashaLoader loader = nuvasha_loader_row_get_loader (row);
  const char *version = nuvasha_loader_row_get_version (row);
  g_autofree char *minecraft =
    instance_string (self->instance, "minecraft-version");
  g_autoptr (GError) error = NULL;

  /* A running game has the loader it started with; the change is not
     written down under it. */
  if (nuvasha_launch_state_is_busy (nuvasha_launcher_get_state (self->launcher)))
    return;

  /* While the row is loading, or when the loader has no version for this
     Minecraft version, there is no choice to store. */
  if (loader != nuvasha_LOADER_VANILLA && version == NULL)
    return;

  /* A load that settled on the version the instance already runs is not
     a change. */
  if (loader == self->loader &&
      g_strcmp0 (loader == nuvasha_LOADER_VANILLA ? NULL : version,
                 self->loader_version) == 0)
    return;

  if (!nuvasha_instance_manager_set_loader (
        self->manager, self->instance, loader,
        loader == nuvasha_LOADER_VANILLA ? NULL : version, &error))
    {
      /* The edit never reached the file: the instance and the row go back
         to the loader the instance runs. */
      nuvasha_instance_set_loader (self->instance, self->loader,
                               self->loader_version, NULL);
      instance_page_show_toast (self, error->message);
      nuvasha_loader_row_load (row, self->settings, self->loader,
                           self->loader_version, minecraft);
      return;
    }

  self->loader = loader;
  g_free (self->loader_version);
  self->loader_version = g_strdup (loader == nuvasha_LOADER_VANILLA
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
java_status_refresh (NuvashaInstancePage *self)
{
  NuvashaJavaRow *row = nuvasha_JAVA_ROW (self->java_row);
  const NuvashaJava *effective = NULL;
  const char *message = NULL;
  GPtrArray *detected;
  g_autofree char *current = NULL;

  if (self->java_row == NULL || !nuvasha_java_row_get_ready (row))
    return;

  detected = nuvasha_java_row_get_detected (row);
  current = instance_string (self->instance, "java-path");

  /* The first entry names the runtime the automatic choice stands for,
     and that same runtime is taken out of the list below so it is not
     shown twice. */
  nuvasha_java_row_set_required_major (row, self->required_java);

  if (detected->len == 0 && current == NULL)
    message = _("No Java installation was found on this system");

  if (message == NULL && self->required_java > 0)
    {
      effective = nuvasha_java_select (detected, current, self->required_java);

      if (effective == NULL && current != NULL)
        message = _("The configured Java was not detected; make sure it "
                     "is still a usable java executable");
      else if (effective != NULL &&
               effective->major_version < self->required_java)
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
        effective = nuvasha_java_select (detected, current, 0);

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
  VersionWait *wait = user_data;
  NuvashaInstancePage *self = wait->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaVersionDescription) description =
    nuvasha_version_load_finish (result, &error);

  if (wait->generation != self->versions_generation)
    {
      version_wait_free (wait);
      return;
    }

  /* The answer is in: the read that brought it is finished with. */
  nuvasha_operation_unref (self->versions_operation);
  self->versions_operation = NULL;

  if (description != NULL && description->java_version != NULL)
    self->required_java = description->java_version->major_version;
  else
    self->required_java = 0;

  java_status_refresh (self);
  version_wait_free (wait);
}

static void
on_manifest_loaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  VersionWait *wait = user_data;
  NuvashaInstancePage *self = wait->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaVersionManifest) manifest =
    nuvasha_version_manifest_fetch_finish (result, &error);
  g_autofree char *version = instance_string (self->instance,
                                              "minecraft-version");

  if (wait->generation != self->versions_generation)
    {
      version_wait_free (wait);
      return;
    }

  /* The manifest is in: the read that brought it is finished with. */
  nuvasha_operation_unref (self->versions_operation);
  self->versions_operation = NULL;

  if (manifest == NULL)
    {
      java_status_refresh (self);
      version_wait_free (wait);
      return;
    }

  self->versions_operation = nuvasha_version_load_async (
    manifest, version, on_description_loaded,
    version_wait_new (self, wait->generation));
  version_wait_free (wait);
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
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
  NuvashaJavaRow *row = nuvasha_JAVA_ROW (object);
  const char *path = nuvasha_java_row_get_selected_path (row);
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

  if (!nuvasha_instance_manager_set_string (self->manager, self->instance,
                                        "java-path", next, &error))
    {
      nuvasha_instance_set_string (self->instance, "java-path", current, NULL);
      instance_page_show_toast (self, error->message);
      nuvasha_java_row_select_path (row, current);
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
update_icon_row (NuvashaInstancePage *self)
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
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
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
        instance_page_show_toast (self, error->message);

      g_object_unref (self);
      return;
    }

  g_autofree char *path = g_file_get_path (file);

  if (path == NULL)
    {
      instance_page_show_toast (self, _("The icon must be a file on this computer"));
      g_object_unref (self);
      return;
    }

  g_autofree char *previous = instance_string (self->instance, "icon");

  if (!nuvasha_instance_manager_set_string (self->manager, self->instance,
                                        "icon", path, &error))
    {
      nuvasha_instance_set_string (self->instance, "icon", previous, NULL);
      instance_page_show_toast (self, error->message);
      g_object_unref (self);
      return;
    }

  update_icon_row (self);
  g_object_unref (self);
}

static void
on_change_icon_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);
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
create_settings_page (NuvashaInstancePage *self)
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
  GtkWidget *version_row = GTK_WIDGET (nuvasha_version_row_new ());
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
  nuvasha_version_row_load (nuvasha_VERSION_ROW (version_row), self->settings,
                        minecraft_version);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (minecraft)), version_row);
  /* The loader and one of its versions: changing either of them changes
     what this instance runs, so both are stored like the version above. */
  {
    NuvashaLoader instance_loader = nuvasha_LOADER_VANILLA;

    g_object_get (self->instance, "loader", &instance_loader, NULL);
    self->loader = instance_loader;
    self->loader_version = g_steal_pointer (&loader_version);
  }

  self->loader_row = nuvasha_loader_row_new ();
  g_signal_connect (self->loader_row, "changed",
                    G_CALLBACK (on_loader_changed), self);
  nuvasha_loader_row_load (self->loader_row, self->settings,
                       self->loader, self->loader_version, minecraft_version);

  /* Rows of the same Minecraft group, drawn and behaving like the version
     row above. */
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (minecraft)),
                             nuvasha_loader_row_get_loader_row (
                               self->loader_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (minecraft)),
                             nuvasha_loader_row_get_version_row (
                               self->loader_row));

  /* Java: the installations detection found. The row ends in an entry
     that adds one it did not report, so that is where they are added
     from. */
  GtkWidget *java_row = GTK_WIDGET (nuvasha_java_row_new ());

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (GTK_WIDGET (java)), _("Java"));

  g_signal_connect (java_row, "notify::selected",
                    G_CALLBACK (on_java_selected), self);
  /* Known before the scan lands, so the first fill already names the
     runtime the automatic choice stands for and leaves it out of the
     list. The required version is not known yet and is set once the
     manifest loads. */
  nuvasha_java_row_set_required_major (nuvasha_JAVA_ROW (java_row),
                                   self->required_java);
  nuvasha_java_row_load (nuvasha_JAVA_ROW (java_row), java_path);
  self->java_row = nuvasha_JAVA_ROW (java_row);

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
  self->versions_operation = nuvasha_version_manifest_fetch_async (
    nuvasha_VERSION_MANIFEST_URL, on_manifest_loaded,
    version_wait_new (self, ++self->versions_generation));

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
on_page_launcher_state (NuvashaLauncher *launcher G_GNUC_UNUSED,
                        NuvashaLaunchState state, const char *detail G_GNUC_UNUSED,
                        gpointer user_data)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (user_data);

  update_directory_row (self);

  if (self->loader_row != NULL)
    nuvasha_loader_row_set_blocked (
      self->loader_row,
      nuvasha_launch_state_is_busy (state));
}

NuvashaInstancePage *
nuvasha_instance_page_new (NuvashaInstanceManager *manager, NuvashaInstance *instance,
                       NuvashaSettings *settings, NuvashaLauncher *launcher)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (manager), NULL);
  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (nuvasha_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (nuvasha_IS_LAUNCHER (launcher), NULL);

  NuvashaInstancePage *self = g_object_new (nuvasha_TYPE_INSTANCE_PAGE, NULL);
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
    GTK_WIDGET (nuvasha_content_page_new (instance, nuvasha_CONTENT_RESOURCE_PACK)),
     "resource-packs", _("Resource Packs"), "image-x-generic-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvasha_content_page_new (instance, nuvasha_CONTENT_SHADER_PACK)),
     "shader-packs", _("Shader Packs"), "applications-graphics-symbolic");
  adw_view_stack_add_titled_with_icon (
    ADW_VIEW_STACK (stack),
    GTK_WIDGET (nuvasha_content_page_new (instance, nuvasha_CONTENT_MOD)),
     "mods", _("Mods"), "view-list-symbolic");

  /* The logs of the instance are the logs of the launcher the window
     shares with the row, so the game can be watched from here. */
  self->logs_page = nuvasha_logs_page_new (launcher);
  g_object_ref (self->logs_page);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (GTK_WIDGET (stack)),
                                        GTK_WIDGET (self->logs_page), "logs", _("Logs"),
                                       "utilities-terminal-symbolic");

  /* Kept so that the content pages can be built again when the loader or
     the Minecraft version changes. */
  self->stack = ADW_VIEW_STACK (stack);
  g_signal_connect (stack, "notify::visible-child",
                    G_CALLBACK (on_content_tab_switched), self);

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
nuvasha_instance_page_dispose (GObject *object)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (object);

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

  nuvasha_operation_cancel (self->scope);
  nuvasha_operation_unref (self->scope);
  self->scope = NULL;

  /* The Java warning's manifest read is this page's: it stops with it. */
  if (self->versions_operation != NULL)
    {
      nuvasha_operation_cancel (self->versions_operation);
      nuvasha_operation_unref (self->versions_operation);
      self->versions_operation = NULL;
    }

  /* The loader rows are the page's. Their handlers go with them, so the
   * rows the widget tree may still hold cannot call back into a page
   * that is on its way out. */
  g_clear_object (&self->loader_row);

  G_OBJECT_CLASS (nuvasha_instance_page_parent_class)->dispose (object);
}

static void
nuvasha_instance_page_finalize (GObject *object)
{
  NuvashaInstancePage *self = nuvasha_INSTANCE_PAGE (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);
  g_clear_object (&self->launcher);
  g_free (self->loader_version);
  g_free (self->export_name);
  g_free (self->export_summary);
  g_free (self->export_version);

  G_OBJECT_CLASS (nuvasha_instance_page_parent_class)->finalize (object);
}

static void
nuvasha_instance_page_class_init (NuvashaInstancePageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_instance_page_dispose;
  object_class->finalize = nuvasha_instance_page_finalize;
}

static void
nuvasha_instance_page_init (NuvashaInstancePage *self)
{
  /* Everything is built in nuvasha_instance_page_new(), which knows the
   * instance the page is for. */
  self->scope = nuvasha_operation_new (NULL, NULL, NULL, NULL);
}
