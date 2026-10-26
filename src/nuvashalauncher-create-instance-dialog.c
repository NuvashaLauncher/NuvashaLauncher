/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The "Create Instance" dialog: one page of rows plus Cancel and Create.
 * Create really builds and persists an instance through
 * nuvashalauncherInstanceManager; a failure is reported inline in the dialog. */

#include "nuvashalauncher-create-instance-dialog.h"
#include "nuvashalauncher-dialogs.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-ui-feedback.h"

#include "nuvashalauncher-loader-row.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-version-row.h"

struct _nuvashalauncherCreateInstanceDialog
{
  AdwDialog parent_instance;

  nuvashalauncherInstanceManager *manager;
  nuvashalauncherSettings *settings;

  /* Directory picked with the file chooser, or NULL to keep the default. */
  char *directory;

  AdwEntryRow *name_row;
  nuvashalauncherVersionRow *version_row;
  nuvashalauncherLoaderRow *loader_row; /* its two rows are filled when it opens */
  AdwActionRow *directory_row;
  AdwPreferencesGroup *group;
  AdwToastOverlay *toast_overlay;
  GtkBox *content;
  GtkButton *create_button;
  GtkButton *cancel_button;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherCreateInstanceDialog, nuvashalauncher_create_instance_dialog,
                     ADW_TYPE_DIALOG)

static void
show_error (nuvashalauncherCreateInstanceDialog *self,
            const char              *message)
{
  nuvashalauncher_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toast_overlay),
                     message);
}

static void
update_create_button (nuvashalauncherCreateInstanceDialog *self)
{
  g_autofree char *name = nuvashalauncher_instance_normalize_name (
    gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  const char *version =
    nuvashalauncher_version_row_get_selected (nuvashalauncher_VERSION_ROW (self->version_row));
  nuvashalauncherLoader loader =
    nuvashalauncher_loader_row_get_loader (self->loader_row);
  gboolean loader_ready = TRUE;

  /* A loader that is not vanilla needs a version of it, and its row only
   * answers when it knows one. */
  if (loader != nuvashalauncher_LOADER_VANILLA)
    loader_ready =
      nuvashalauncher_loader_row_get_version (self->loader_row) != NULL;

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
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (user_data);
  const char *version =
    nuvashalauncher_version_row_get_selected (nuvashalauncher_VERSION_ROW (self->version_row));

  /* While the Minecraft list loads, or when it failed, there is no
     version to tell the loader row about. */
  if (version == NULL)
    {
      update_create_button (self);
      return;
    }

  nuvashalauncher_loader_row_load (self->loader_row, self->settings,
                       nuvashalauncher_loader_row_get_loader (self->loader_row),
                       NULL, version);
  update_create_button (self);
}

static void
on_loader_changed (nuvashalauncherLoaderRow *row G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (user_data);

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
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (user_data);

  update_create_button (self);
}

static void
on_create_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (user_data);
  g_autofree char *name = nuvashalauncher_instance_normalize_name (
    gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  const char *version =
    nuvashalauncher_version_row_get_selected (nuvashalauncher_VERSION_ROW (self->version_row));
  nuvashalauncherLoader loader =
    nuvashalauncher_loader_row_get_loader (self->loader_row);
  const char *loader_version =
    nuvashalauncher_loader_row_get_version (self->loader_row);
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;

  /* The Create button is insensitive without a version, so this only
   * guards against the row being emptied underneath it. */
  if (version == NULL)
    {
      show_error (self, _("Pick a Minecraft version from the list."));
      return;
    }

  if (loader != nuvashalauncher_LOADER_VANILLA && loader_version == NULL)
    {
      show_error (self,
                  _("Pick a version of the loader, or pick another loader."));
      return;
    }

  instance = nuvashalauncher_instance_manager_create (self->manager, name, self->directory,
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
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (user_data);
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
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();

  gtk_file_dialog_select_folder (dialog,
                                 GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                                 NULL, on_folder_selected,
                                 g_object_ref (self));
}

static void
nuvashalauncher_create_instance_dialog_init (nuvashalauncherCreateInstanceDialog *self)
{
  g_autofree char *default_directory =
    g_build_filename (g_get_user_data_dir (), "nuvashalauncher", "instances", NULL);
  GtkButton *pick_button;

  gtk_widget_init_template (GTK_WIDGET (self));
  gtk_widget_set_size_request (GTK_WIDGET (self), 360, 200);
  nuvashalauncher_dialog_add_content_margin_breakpoint (ADW_DIALOG (self),
                                                GTK_WIDGET (self->content),
                                                600, 6);
  adw_dialog_set_follows_content_size (ADW_DIALOG (self), TRUE);
  adw_dialog_set_content_width (ADW_DIALOG (self), 560);
  adw_entry_row_set_activates_default (self->name_row, TRUE);
  adw_entry_row_set_max_length (self->name_row, 16);

  self->version_row = nuvashalauncher_version_row_new ();
  self->loader_row = nuvashalauncher_loader_row_new ();
  self->directory_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->directory_row), _("Directory"));
  adw_action_row_set_subtitle (self->directory_row, default_directory);
  pick_button = GTK_BUTTON (gtk_button_new_from_icon_name ("folder-symbolic"));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (pick_button), _("Choose a folder"));
  g_signal_connect_object (pick_button, "clicked", G_CALLBACK (on_pick_directory), self, 0);
  adw_action_row_add_suffix (self->directory_row, GTK_WIDGET (pick_button));
  adw_action_row_set_activatable_widget (self->directory_row, GTK_WIDGET (pick_button));

  adw_preferences_group_add (self->group, GTK_WIDGET (self->version_row));
  adw_preferences_group_add (self->group, GTK_WIDGET (nuvashalauncher_loader_row_get_loader_row (self->loader_row)));
  adw_preferences_group_add (self->group, GTK_WIDGET (nuvashalauncher_loader_row_get_version_row (self->loader_row)));
  adw_preferences_group_add (self->group, GTK_WIDGET (self->directory_row));

  gtk_button_set_label (self->cancel_button, _("Cancel"));
  gtk_button_set_label (self->create_button, _("Create"));
  gtk_widget_add_css_class (GTK_WIDGET (self->create_button), "suggested-action");
  g_signal_connect_object (self->cancel_button, "clicked", G_CALLBACK (nuvashalauncher_ui_cancel_dialog), self, 0);
  g_signal_connect_object (self->create_button, "clicked", G_CALLBACK (on_create_clicked), self, 0);
  g_signal_connect_object (self->name_row, "notify::text", G_CALLBACK (on_input_changed), self, 0);
  g_signal_connect_object (self->version_row, "notify::selected", G_CALLBACK (on_version_selected), self, 0);
  g_signal_connect_object (self->loader_row, "changed", G_CALLBACK (on_loader_changed), self, 0);
  adw_dialog_set_default_widget (ADW_DIALOG (self), GTK_WIDGET (self->create_button));
  update_create_button (self);
}

nuvashalauncherCreateInstanceDialog *
nuvashalauncher_create_instance_dialog_new (nuvashalauncherInstanceManager *manager,
                                nuvashalauncherSettings        *settings)
{
  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE_MANAGER (manager), NULL);
  g_return_val_if_fail (nuvashalauncher_IS_SETTINGS (settings), NULL);

  nuvashalauncherCreateInstanceDialog *self =
    g_object_new (nuvashalauncher_TYPE_CREATE_INSTANCE_DIALOG, NULL);

  self->manager = g_object_ref (manager);
  self->settings = g_object_ref (settings);

  /* The widget tree is built before there are settings to ask, so the
   * versions are fetched now. Nothing is picked until the user picks
   * it: the row knows no version of its own. */
  nuvashalauncher_version_row_load (nuvashalauncher_VERSION_ROW (self->version_row), self->settings,
                        NULL);

  /* Vanilla is picked from the start: it is what an instance is without a
     loader, and it has no versions of its own to wait for. */
  nuvashalauncher_loader_row_load (self->loader_row, self->settings,
                       nuvashalauncher_LOADER_VANILLA, NULL, NULL);

  return self;
}

static void
nuvashalauncher_create_instance_dialog_dispose (GObject *object)
{
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->settings);

  /* The loader rows are the dialog's; their handlers go with them. */
  g_clear_object (&self->loader_row);

  G_OBJECT_CLASS (nuvashalauncher_create_instance_dialog_parent_class)->dispose (object);
}

static void
nuvashalauncher_create_instance_dialog_finalize (GObject *object)
{
  nuvashalauncherCreateInstanceDialog *self = nuvashalauncher_CREATE_INSTANCE_DIALOG (object);

  g_free (self->directory);

  G_OBJECT_CLASS (nuvashalauncher_create_instance_dialog_parent_class)->finalize (object);
}

static void
nuvashalauncher_create_instance_dialog_class_init (nuvashalauncherCreateInstanceDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  gtk_widget_class_set_template_from_resource (widget_class,
      "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-create-instance.ui");
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherCreateInstanceDialog, name_row);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherCreateInstanceDialog, group);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherCreateInstanceDialog, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherCreateInstanceDialog, content);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherCreateInstanceDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherCreateInstanceDialog, create_button);

  object_class->dispose = nuvashalauncher_create_instance_dialog_dispose;
  object_class->finalize = nuvashalauncher_create_instance_dialog_finalize;
}
