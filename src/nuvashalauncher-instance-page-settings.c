/* Settings page and editable instance controls. */
#include "nuvashalauncher-instance-page-private.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-content-page.h"
#include "nuvashalauncher-version-row.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-task.h"
#include "nuvashalauncher-operation.h"
#include "nuvashalauncher-ui-feedback.h"

/* ---------------------------------------------------------- *
 * Saving the editable rows
 * ---------------------------------------------------------- */

static void
on_name_apply (AdwEntryRow *row, gpointer user_data)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (row));
  g_autofree char *current = instance_string (self->instance, "name");
  g_autoptr (GError) error = NULL;

  if (g_strcmp0 (text, current) == 0)
    return;

  if (!nuvashalauncher_instance_manager_rename (self->manager, self->instance, text,
                                    &error))
    {
      /* The edit never reached the file: the row shows the stored name
       * again instead of a name that does not exist. */
      instance_page_show_error (self, error);
      gtk_editable_set_text (GTK_EDITABLE (row), current);
      return;
    }

  g_autofree char *renamed = instance_string (self->instance, "name");

  gtk_editable_set_text (GTK_EDITABLE (row), renamed);

  /* The tab follows the name: the window watches the instance for this,
     because the tab is the window's and not the page's. */
  refresh_in_list (self);
}

static void
on_jvm_args_apply (AdwEntryRow *row, gpointer user_data)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (row));
  g_autofree char *current = instance_string (self->instance, "jvm-args");
  g_autoptr (GError) error = NULL;

  if (g_strcmp0 (text, current) == 0)
    return;

  /* nuvashalauncherInstanceManager validates before it writes, so an empty line
   * fails here and never reaches instances.json. */
  if (!nuvashalauncher_instance_manager_set_string (self->manager, self->instance,
                                        "jvm-args", text, &error))
    {
      nuvashalauncher_instance_set_string (self->instance, "jvm-args", current, NULL);
      instance_page_show_error (self, error);
      gtk_editable_set_text (GTK_EDITABLE (row), current);
      return;
    }
}

typedef struct
{
  nuvashalauncherInstancePage *page;
  guint generation;
} VersionWait;

static void
version_wait_free (VersionWait *wait)
{
  g_object_unref (wait->page);
  g_free (wait);
}

static VersionWait *
version_wait_new (nuvashalauncherInstancePage *self, guint generation)
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

/* Store a selected version only after the version list has loaded. */
static void
on_version_selected (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);
  nuvashalauncherVersionRow *row = nuvashalauncher_VERSION_ROW (object);
  const char *version = nuvashalauncher_version_row_get_selected (row);
  g_autofree char *current =
    instance_string (self->instance, "minecraft-version");
  g_autoptr (GError) error = NULL;

  if (version == NULL)
    return;

  if (g_strcmp0 (version, current) == 0)
    return;

  if (!nuvashalauncher_instance_manager_set_string (self->manager, self->instance,
                                        "minecraft-version", version,
                                        &error))
    {
      /* The edit never reached the file: the instance and the row go
       * back to the version that is still stored. */
      nuvashalauncher_instance_set_string (self->instance, "minecraft-version", current,
                               NULL);
      instance_page_show_error (self, error);
      nuvashalauncher_version_row_select (row, current);
      return;
    }

  /* The row shows the version this instance runs. */
  refresh_in_list (self);

  /* Resource Packs and Shader Packs are narrowed to the Minecraft
     version, so they are built again for the version that was picked. */
  rebuild_content_pages (self);

  /* A loader has one set of versions per Minecraft version, so the list
     is asked again for the version that was picked. */
  nuvashalauncher_loader_row_load (self->loader_row, self->settings,
                       nuvashalauncher_loader_row_get_loader (self->loader_row),
                       self->loader_version, version);

  /* Its javaVersion requirement may differ: check the compatibility
   * warning again, using the manifest cache. */
  if (self->versions_operation != NULL)
    {
      nuvashalauncher_operation_cancel (self->versions_operation);
      nuvashalauncher_operation_unref (self->versions_operation);
    }
  self->versions_operation = nuvashalauncher_version_manifest_fetch_async (
    nuvashalauncher_VERSION_MANIFEST_URL, on_manifest_loaded,
    version_wait_new (self, ++self->versions_generation));
}


/* ---------------------------------------------------------- *
 * The loader and the version of it
 * ---------------------------------------------------------- */

/* The loader and its version are what the instance runs, so they are
   written through the manager the way the Minecraft version above is. A
   version the row answers nothing for is nothing to store. */
static void
on_loader_changed (nuvashalauncherLoaderRow *row, gpointer user_data)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);
  nuvashalauncherLoader loader = nuvashalauncher_loader_row_get_loader (row);
  const char *version = nuvashalauncher_loader_row_get_version (row);
  g_autofree char *minecraft =
    instance_string (self->instance, "minecraft-version");
  g_autoptr (GError) error = NULL;

  /* A running game has the loader it started with; the change is not
     written down under it. */
  if (nuvashalauncher_launch_state_is_busy (nuvashalauncher_launcher_get_state (self->launcher)))
    return;

  /* While the row is loading, or when the loader has no version for this
     Minecraft version, there is no choice to store. */
  if (loader != nuvashalauncher_LOADER_VANILLA && version == NULL)
    return;

  /* A load that settled on the version the instance already runs is not
     a change. */
  if (loader == self->loader &&
      g_strcmp0 (loader == nuvashalauncher_LOADER_VANILLA ? NULL : version,
                 self->loader_version) == 0)
    return;

  if (!nuvashalauncher_instance_manager_set_loader (
        self->manager, self->instance, loader,
        loader == nuvashalauncher_LOADER_VANILLA ? NULL : version, &error))
    {
      /* The edit never reached the file: the instance and the row go back
         to the loader the instance runs. */
      nuvashalauncher_instance_set_loader (self->instance, self->loader,
                               self->loader_version, NULL);
      instance_page_show_error (self, error);
      nuvashalauncher_loader_row_load (row, self->settings, self->loader,
                           self->loader_version, minecraft);
      return;
    }

  self->loader = loader;
  g_free (self->loader_version);
  self->loader_version = g_strdup (loader == nuvashalauncher_LOADER_VANILLA
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
java_status_refresh (nuvashalauncherInstancePage *self)
{
  nuvashalauncherJavaRow *row = nuvashalauncher_JAVA_ROW (self->java_row);
  const nuvashalauncherJava *effective = NULL;
  const char *message = NULL;
  GPtrArray *detected;
  const char *selected_path;
  g_autofree char *current = NULL;

  if (self->java_row == NULL || !nuvashalauncher_java_row_get_ready (row))
    return;

  detected = nuvashalauncher_java_row_get_detected (row);
  current = instance_string (self->instance, "java-path");

  /* The first entry names the runtime the automatic choice stands for,
     and that same runtime is taken out of the list below so it is not
     shown twice. */
  nuvashalauncher_java_row_set_required_major (row, self->required_java);
  selected_path = nuvashalauncher_java_row_get_selected_path (row);

   if (message == NULL && self->required_java > 0)
    {
      effective = nuvashalauncher_java_select (detected, current, self->required_java);

       if (effective == NULL && current != NULL &&
           !g_file_test (current, G_FILE_TEST_IS_EXECUTABLE) &&
           !(selected_path != NULL && *selected_path != '\0' &&
             g_strcmp0 (selected_path, current) == 0 &&
             g_file_test (selected_path, G_FILE_TEST_IS_EXECUTABLE)))
        message = _("The configured Java was not detected; make sure it "
                     "is still a usable java executable");
      else if (effective != NULL &&
               effective->major_version < self->required_java)
        {
          g_autofree char *description = g_strdup_printf (
            _("This game needs Java %d or newer, selected Java is %d"),
            self->required_java, effective->major_version);

          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), description);
          return;
        }
    }

  /* A usable JVM needs AWT's X11 bridge; warn for a headless runtime
   * even when nothing else is wrong with the selection. */
  if (message == NULL)
    {
      if (effective == NULL)
        effective = nuvashalauncher_java_select (detected, current, 0);

      if (effective != NULL && effective->headless)
        message = _("Select a Java runtime with desktop support, not a headless installation.");
    }

  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), message);
}

static void
on_description_loaded (GObject *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer user_data)
{
  VersionWait *wait = user_data;
  nuvashalauncherInstancePage *self = wait->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionDescription) description =
    nuvashalauncher_version_load_finish (result, &error);

  if (wait->generation != self->versions_generation)
    {
      version_wait_free (wait);
      return;
    }

  /* The answer is in: the read that brought it is finished with. */
  nuvashalauncher_operation_unref (self->versions_operation);
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
  nuvashalauncherInstancePage *self = wait->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionManifest) manifest =
    nuvashalauncher_version_manifest_fetch_finish (result, &error);
  g_autofree char *version = instance_string (self->instance,
                                              "minecraft-version");

  if (wait->generation != self->versions_generation)
    {
      version_wait_free (wait);
      return;
    }

  /* The manifest is in: the read that brought it is finished with. */
  nuvashalauncher_operation_unref (self->versions_operation);
  self->versions_operation = NULL;

  if (manifest == NULL)
    {
      java_status_refresh (self);
      version_wait_free (wait);
      return;
    }

  self->versions_operation = nuvashalauncher_version_load_async (
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
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);
  nuvashalauncherJavaRow *row = nuvashalauncher_JAVA_ROW (object);
  const char *path = nuvashalauncher_java_row_get_selected_path (row);
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

  if (!nuvashalauncher_instance_manager_set_string (self->manager, self->instance,
                                        "java-path", next, &error))
    {
      nuvashalauncher_instance_set_string (self->instance, "java-path", current, NULL);
      instance_page_show_error (self, error);
      nuvashalauncher_java_row_select_path (row, current);
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
  gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (GTK_LABEL (text), 30);

  gtk_widget_set_valign (GTK_WIDGET (image), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (text), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (arrow), GTK_ALIGN_CENTER);

  gtk_box_append (GTK_BOX (box), GTK_WIDGET (image));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (text));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (arrow));
  gtk_button_set_child (GTK_BUTTON (button), GTK_WIDGET (box));

  if (label != NULL)
    *label = GTK_LABEL (text);

  return GTK_WIDGET (button);
}

static void
update_icon_row (nuvashalauncherInstancePage *self)
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
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);
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
        instance_page_show_error (self, error);

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

  if (!nuvashalauncher_instance_manager_set_string (self->manager, self->instance,
                                        "icon", path, &error))
    {
      nuvashalauncher_instance_set_string (self->instance, "icon", previous, NULL);
      instance_page_show_error (self, error);
      g_object_unref (self);
      return;
    }

  update_icon_row (self);
  g_object_unref (self);
}

static void
on_change_icon_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherInstancePage *self = nuvashalauncher_INSTANCE_PAGE (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  gtk_file_dialog_set_title (dialog, _("Choose an Icon"));
   gtk_file_filter_set_name (filter, _("Images"));
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

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);

  gtk_widget_set_valign (GTK_WIDGET (button), GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (button), title);

  g_signal_connect_object (button, "clicked", callback, user_data, 0);

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (button));
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), GTK_WIDGET (button));
  adw_preferences_group_add (group, GTK_WIDGET (row));

  return GTK_WIDGET (row);
}


/* ---------------------------------------------------------- *
 * The five pages
 * ---------------------------------------------------------- */

GtkWidget *
create_settings_page (nuvashalauncherInstancePage *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  gtk_widget_add_css_class (GTK_WIDGET (page), "nuvashalauncher-settings-page");
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
  GtkWidget *version_row = GTK_WIDGET (nuvashalauncher_version_row_new ());
  AdwEntryRow *jvm_args_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  AdwPreferencesGroup *general = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *minecraft = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *java = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *actions = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  gtk_widget_add_css_class (GTK_WIDGET (general), "nuvashalauncher-preferences-surface");
  gtk_widget_add_css_class (GTK_WIDGET (minecraft), "nuvashalauncher-preferences-surface");
  gtk_widget_add_css_class (GTK_WIDGET (java), "nuvashalauncher-preferences-surface");
  gtk_widget_add_css_class (GTK_WIDGET (actions), "nuvashalauncher-preferences-surface");
  GtkWidget *change_icon = NULL;
  GtkWidget *delete_row = NULL;

  /* Names, paths and arguments are text from the user or the disk: none
     of them is markup. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (name_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (jvm_args_row),
                                      FALSE);

  gtk_widget_set_vexpand (GTK_WIDGET (page), TRUE);

  /* General */
  adw_preferences_group_set_title (general, _("General"));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_row), _("Name"));
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (name_row), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (name_row), name);
  g_signal_connect_object (name_row, "apply", G_CALLBACK (on_name_apply), self, 0);
  adw_preferences_group_add (general, GTK_WIDGET (name_row));

  self->icon_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->icon_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->icon_row), _("Icon"));

  /* The button is the path and the chooser at once: pressing it picks a
     file, and what it picked is written in it. */
  change_icon = path_button (NULL, &self->icon_path_label);
  gtk_widget_set_valign (change_icon, GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (change_icon, _("Choose an Icon"));
  g_signal_connect_object (change_icon, "clicked",
                           G_CALLBACK (on_change_icon_clicked), self, 0);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->icon_row), change_icon);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->icon_row),
                                         change_icon);
  adw_preferences_group_add (general, GTK_WIDGET (self->icon_row));
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
   gtk_widget_set_valign (GTK_WIDGET (self->directory_change), GTK_ALIGN_CENTER);
   nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (self->directory_change),
                                   _("Change the Directory"));
   g_signal_connect_object (self->directory_change, "clicked",
                            G_CALLBACK (on_change_directory_clicked), self, 0);

  self->directory_spinner = ADW_SPINNER (adw_spinner_new ());
  gtk_widget_set_valign (GTK_WIDGET (self->directory_spinner), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->directory_spinner), FALSE);

  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             GTK_WIDGET (self->directory_spinner));
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             GTK_WIDGET (self->directory_change));
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->directory_row),
                                         GTK_WIDGET (self->directory_change));
  adw_preferences_group_add (general,
                             GTK_WIDGET (self->directory_row));
  update_directory_row (self);

  /* Minecraft: the list of versions comes from Mojang's manifest,
   * fetched as the page is built. The version this instance runs stays
   * in the list even when the filter would hide its type. */
  adw_preferences_group_set_title (minecraft,
                                    _("Minecraft"));
  g_signal_connect_object (version_row, "notify::selected",
                           G_CALLBACK (on_version_selected), self, 0);
  nuvashalauncher_version_row_load (nuvashalauncher_VERSION_ROW (version_row), self->settings,
                        minecraft_version);
  adw_preferences_group_add (minecraft, version_row);
  /* The loader and one of its versions: changing either of them changes
     what this instance runs, so both are stored like the version above. */
  {
    nuvashalauncherLoader instance_loader = nuvashalauncher_LOADER_VANILLA;

    g_object_get (self->instance, "loader", &instance_loader, NULL);
    self->loader = instance_loader;
    self->loader_version = g_steal_pointer (&loader_version);
  }

  self->loader_row = nuvashalauncher_loader_row_new ();
  g_signal_connect_object (self->loader_row, "changed",
                           G_CALLBACK (on_loader_changed), self, 0);
  nuvashalauncher_loader_row_load (self->loader_row, self->settings,
                       self->loader, self->loader_version, minecraft_version);

  /* Rows of the same Minecraft group, drawn and behaving like the version
     row above. */
  adw_preferences_group_add (minecraft,
                              GTK_WIDGET (nuvashalauncher_loader_row_get_loader_row (
                                self->loader_row)));
  adw_preferences_group_add (minecraft,
                              GTK_WIDGET (nuvashalauncher_loader_row_get_version_row (
                                self->loader_row)));

  /* Java: the installations detection found. The row ends in an entry
     that adds one it did not report, so that is where they are added
     from. */
  GtkWidget *java_row = GTK_WIDGET (nuvashalauncher_java_row_new ());

  adw_preferences_group_set_title (java, _("Java"));

  g_signal_connect_object (java_row, "notify::selected",
                           G_CALLBACK (on_java_selected), self, 0);
  /* Known before the scan lands, so the first fill already names the
     runtime the automatic choice stands for and leaves it out of the
     list. The required version is not known yet and is set once the
     manifest loads. */
  nuvashalauncher_java_row_set_required_major (nuvashalauncher_JAVA_ROW (java_row),
                                   self->required_java);
  nuvashalauncher_java_row_load (nuvashalauncher_JAVA_ROW (java_row), java_path);
  self->java_row = nuvashalauncher_JAVA_ROW (java_row);

  adw_preferences_group_add (java, java_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (jvm_args_row),
                                 _("JVM Arguments"));
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (jvm_args_row), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (jvm_args_row), jvm_args);
  g_signal_connect_object (jvm_args_row, "apply", G_CALLBACK (on_jvm_args_apply),
                           self, 0);
  adw_preferences_group_add (java, GTK_WIDGET (jvm_args_row));

  /* The Java row's compatibility warning needs the javaVersion field of
   * the instance's version document; fetching is offline-friendly via
   * the cache. */
  self->versions_operation = nuvashalauncher_version_manifest_fetch_async (
    nuvashalauncher_VERSION_MANIFEST_URL, on_manifest_loaded,
    version_wait_new (self, ++self->versions_generation));

  /* Actions */
  adw_preferences_group_set_title (actions, _("Actions"));
  button_row (actions, _("Duplicate"), NULL,
              "edit-copy-symbolic", G_CALLBACK (on_duplicate_clicked), self);
  button_row (actions, _("Open Folder"), NULL,
              "folder-symbolic", G_CALLBACK (on_open_folder_clicked),
              self);
  button_row (actions, _("Export Modpack"),
              NULL,
              "document-save-symbolic", G_CALLBACK (on_export_clicked), self);
  delete_row = button_row (actions, _("Delete Instance"),
                           NULL, "user-trash-symbolic",
                           G_CALLBACK (on_delete_clicked), self);
  gtk_widget_add_css_class (
    adw_action_row_get_activatable_widget (ADW_ACTION_ROW (delete_row)),
    "destructive-action");

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            general);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            minecraft);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            java);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            actions);

  return GTK_WIDGET (page);
}
