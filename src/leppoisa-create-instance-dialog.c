/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The "Create Instance" dialog: one page of rows plus Cancel and Create.
 * Create really builds and persists an instance through
 * LeppoisaInstanceManager; a failure is reported inline in the dialog. */

#include "leppoisa-create-instance-dialog.h"
#include "leppoisa-i18n.h"
#include "leppoisa-ui-feedback.h"

#include "leppoisa-loader-row.h"
#include "leppoisa-loaders.h"
#include "leppoisa-version-row.h"

struct _LeppoisaCreateInstanceDialog
{
  AdwDialog parent_instance;

  LeppoisaInstanceManager *manager;
  LeppoisaSettings *settings;

  /* Directory picked with the file chooser, or NULL to keep the default. */
  char *directory;

  AdwEntryRow *name_row;
  LeppoisaVersionRow *version_row;
  LeppoisaLoaderRow *loader_row; /* its two rows are filled when it opens */
  AdwActionRow *directory_row;
  AdwToastOverlay *toast_overlay;
  GtkButton *create_button;
};

G_DEFINE_FINAL_TYPE (LeppoisaCreateInstanceDialog, leppoisa_create_instance_dialog,
                     ADW_TYPE_DIALOG)

static void
show_error (LeppoisaCreateInstanceDialog *self,
            const char              *message)
{
  leppoisa_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toast_overlay),
                     message);
}

static void
update_create_button (LeppoisaCreateInstanceDialog *self)
{
  g_autofree char *name = leppoisa_instance_normalize_name (
    gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  const char *version =
    leppoisa_version_row_get_selected (leppoisa_VERSION_ROW (self->version_row));
  LeppoisaLoader loader =
    leppoisa_loader_row_get_loader (self->loader_row);
  gboolean loader_ready = TRUE;

  /* A loader that is not vanilla needs a version of it, and its row only
   * answers when it knows one. */
  if (loader != leppoisa_LOADER_VANILLA)
    loader_ready =
      leppoisa_loader_row_get_version (self->loader_row) != NULL;

  /* The version row answers nothing while it is loading or failed, so
   * the dialog waits until a version from the list is picked. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->create_button),
                            name[0] != '\0' && version != NULL &&
                            loader_ready);
}

/* A loader needs a Minecraft version to say which of its versions run it,
   so the loader row is told which version is picked whenever that
   changes. */
static void
on_version_selected (GObject *object G_GNUC_UNUSED,
                     GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (user_data);
  const char *version =
    leppoisa_version_row_get_selected (leppoisa_VERSION_ROW (self->version_row));

  /* While the Minecraft list loads, or when it failed, there is no
     version to tell the loader row about. */
  if (version == NULL)
    {
      update_create_button (self);
      return;
    }

  leppoisa_loader_row_load (self->loader_row, self->settings,
                       leppoisa_loader_row_get_loader (self->loader_row),
                       NULL, version);
  update_create_button (self);
}

static void
on_loader_changed (LeppoisaLoaderRow *row G_GNUC_UNUSED, gpointer user_data)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (user_data);

  /* The row reloads its own versions when the loader changes and tells
     this handler again once one is picked; the dialog only has to say
     whether Create may answer. */
  update_create_button (self);
}

static void
on_input_changed (GObject *object G_GNUC_UNUSED,
                  GParamSpec *pspec G_GNUC_UNUSED,
                  gpointer user_data)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (user_data);

  update_create_button (self);
}

static void
on_create_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (user_data);
  g_autofree char *name = leppoisa_instance_normalize_name (
    gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  const char *version =
    leppoisa_version_row_get_selected (leppoisa_VERSION_ROW (self->version_row));
  LeppoisaLoader loader =
    leppoisa_loader_row_get_loader (self->loader_row);
  const char *loader_version =
    leppoisa_loader_row_get_version (self->loader_row);
  g_autoptr (GError) error = NULL;
  g_autoptr (LeppoisaInstance) instance = NULL;

  /* The Create button is insensitive without a version, so this only
   * guards against the row being emptied underneath it. */
  if (version == NULL)
    {
      show_error (self, "Pick a Minecraft version from the list.");
      return;
    }

  if (loader != leppoisa_LOADER_VANILLA && loader_version == NULL)
    {
      show_error (self,
                  "Pick a version of the loader, or pick another loader.");
      return;
    }

  instance = leppoisa_instance_manager_create (self->manager, name, self->directory,
                                          version, loader, loader_version,
                                          &error);
  if (instance == NULL)
    {
      show_error (self, error->message);
      return;
    }

  adw_dialog_close (ADW_DIALOG (self));
}

static void
on_folder_selected (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), result,
                                          &error);

  if (file == NULL)
    {
      /* Dismissing the chooser is not a failure. */
      if (error == NULL ||
          g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
          g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        {
          g_object_unref (self);
          return;
        }

      show_error (self, error->message);
      g_object_unref (self);
      return;
    }

  g_free (self->directory);
  self->directory = g_file_get_path (file);

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->directory_row),
                               self->directory);
  g_object_unref (self);
}

static void
on_pick_directory (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();

  gtk_file_dialog_select_folder (dialog,
                                 GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                                 NULL, on_folder_selected,
                                 g_object_ref (self));
}

static void
leppoisa_create_instance_dialog_init (LeppoisaCreateInstanceDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), _("Create Instance"));
  adw_dialog_set_follows_content_size (ADW_DIALOG (self), FALSE);

  self->name_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name_row), _("Name"));
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (self->name_row), TRUE);

  /* The real versions of Mojang's manifest, fetched when the dialog is
   * created; the row fills itself and says so while it works. */
  self->version_row = leppoisa_version_row_new ();

  /* The loader and the version of it. Filled when the dialog opens and
     again whenever the Minecraft version above changes. */
  self->loader_row = leppoisa_loader_row_new ();

  /* What LeppoisaInstanceManager derives when no directory is picked:
   * <data dir>/instances. */
  g_autofree char *default_directory =
    g_build_filename (g_get_user_data_dir (), "leppoisa", "instances", NULL);

  self->directory_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->directory_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->directory_row),
                                  _("Directory"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->directory_row),
                               default_directory);

  GtkButton *pick_button = GTK_BUTTON (gtk_button_new_from_icon_name ("folder-symbolic"));
  gtk_widget_add_css_class (GTK_WIDGET (pick_button), "flat");
  gtk_widget_set_valign (GTK_WIDGET (pick_button), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (GTK_WIDGET (pick_button), _("Choose a folder"));
  g_signal_connect (pick_button, "clicked", G_CALLBACK (on_pick_directory), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row), GTK_WIDGET (pick_button));
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->directory_row),
                                         GTK_WIDGET (pick_button));

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (group)), GTK_WIDGET (self->name_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (group)), GTK_WIDGET (self->version_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (group)),
                             leppoisa_loader_row_get_loader_row (self->loader_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (group)),
                             leppoisa_loader_row_get_version_row (self->loader_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (group)), GTK_WIDGET (self->directory_row));

  GtkButton *cancel_button = GTK_BUTTON (gtk_button_new_with_label (_("Cancel")));
  g_signal_connect (cancel_button, "clicked", G_CALLBACK (leppoisa_ui_cancel_dialog),
                    self);

  self->create_button = GTK_BUTTON (gtk_button_new_with_label (_("Create")));
  gtk_widget_add_css_class (GTK_WIDGET (self->create_button), "suggested-action");
  g_signal_connect (self->create_button, "clicked", G_CALLBACK (on_create_clicked),
                    self);

  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (cancel_button));
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (self->create_button));

  GtkBox *content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_set_spacing (content, 12);
  gtk_widget_set_margin_start (GTK_WIDGET (content), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (content), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (content), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (content), 12);
  GtkLabel *heading = GTK_LABEL (gtk_label_new (_("Create Instance")));
  gtk_widget_add_css_class (GTK_WIDGET (heading), "title-2");
  gtk_label_set_xalign (GTK_LABEL (heading), 0.5f);
  gtk_box_append (GTK_BOX (GTK_WIDGET (content)), GTK_WIDGET (heading));
  gtk_box_append (GTK_BOX (GTK_WIDGET (content)), GTK_WIDGET (group));
  gtk_box_append (GTK_BOX (GTK_WIDGET (content)), GTK_WIDGET (buttons));
  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toast_overlay, GTK_WIDGET (content));
  adw_dialog_set_child (ADW_DIALOG (self), GTK_WIDGET (self->toast_overlay));

  adw_dialog_set_default_widget (ADW_DIALOG (self),
                                 GTK_WIDGET (self->create_button));

  g_signal_connect (self->name_row, "notify::text", G_CALLBACK (on_input_changed),
                    self);
  g_signal_connect (self->version_row, "notify::selected",
                    G_CALLBACK (on_version_selected), self);
  g_signal_connect (self->loader_row, "changed",
                    G_CALLBACK (on_loader_changed), self);

  update_create_button (self);
}

LeppoisaCreateInstanceDialog *
leppoisa_create_instance_dialog_new (LeppoisaInstanceManager *manager,
                                LeppoisaSettings        *settings)
{
  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (manager), NULL);
  g_return_val_if_fail (leppoisa_IS_SETTINGS (settings), NULL);

  LeppoisaCreateInstanceDialog *self =
    g_object_new (leppoisa_TYPE_CREATE_INSTANCE_DIALOG, NULL);

  self->manager = g_object_ref (manager);
  self->settings = g_object_ref (settings);

  /* The widget tree is built before there are settings to ask, so the
   * versions are fetched now. Nothing is picked until the user picks
   * it: the row knows no version of its own. */
  leppoisa_version_row_load (leppoisa_VERSION_ROW (self->version_row), self->settings,
                        NULL);

  /* Vanilla is picked from the start: it is what an instance is without a
     loader, and it has no versions of its own to wait for. */
  leppoisa_loader_row_load (self->loader_row, self->settings,
                       leppoisa_LOADER_VANILLA, NULL, NULL);

  return self;
}

static void
leppoisa_create_instance_dialog_dispose (GObject *object)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->settings);

  /* The loader rows are the dialog's; their handlers go with them. */
  g_clear_object (&self->loader_row);

  G_OBJECT_CLASS (leppoisa_create_instance_dialog_parent_class)->dispose (object);
}

static void
leppoisa_create_instance_dialog_finalize (GObject *object)
{
  LeppoisaCreateInstanceDialog *self = leppoisa_CREATE_INSTANCE_DIALOG (object);

  g_free (self->directory);

  G_OBJECT_CLASS (leppoisa_create_instance_dialog_parent_class)->finalize (object);
}

static void
leppoisa_create_instance_dialog_class_init (LeppoisaCreateInstanceDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = leppoisa_create_instance_dialog_dispose;
  object_class->finalize = leppoisa_create_instance_dialog_finalize;
}
