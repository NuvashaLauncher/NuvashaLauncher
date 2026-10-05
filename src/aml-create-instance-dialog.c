/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The "Create Instance" dialog: one page of rows plus Cancel and Create.
 * Create really builds and persists an instance through
 * AmlInstanceManager; a failure is reported inline in the dialog. */

#include "aml-create-instance-dialog.h"

struct _AmlCreateInstanceDialog
{
  AdwDialog parent_instance;

  AmlInstanceManager *manager;

  /* Directory picked with the file chooser, or NULL to keep the default. */
  char *directory;

  GtkWidget *name_row;
  GtkWidget *version_row;
  GtkWidget *directory_row;
  GtkWidget *error_label;
  GtkWidget *create_button;
};

G_DEFINE_FINAL_TYPE (AmlCreateInstanceDialog, aml_create_instance_dialog,
                     ADW_TYPE_DIALOG)

static void
show_error (AmlCreateInstanceDialog *self,
            const char              *message)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), message);
  gtk_widget_set_visible (self->error_label, TRUE);
}

static void
clear_error (AmlCreateInstanceDialog *self)
{
  gtk_widget_set_visible (self->error_label, FALSE);
}

static void
update_create_button (AmlCreateInstanceDialog *self)
{
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autofree char *version =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->version_row)));

  g_strstrip (name);
  g_strstrip (version);

  gtk_widget_set_sensitive (self->create_button,
                            name[0] != '\0' && version[0] != '\0');
}

static void
on_input_changed (GObject *object G_GNUC_UNUSED,
                  GParamSpec *pspec G_GNUC_UNUSED,
                  gpointer user_data)
{
  AmlCreateInstanceDialog *self = user_data;

  clear_error (self);
  update_create_button (self);
}

static void
on_cancel_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  adw_dialog_close (ADW_DIALOG (user_data));
}

static void
on_create_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlCreateInstanceDialog *self = user_data;
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autofree char *version =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->version_row)));
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlInstance) instance = NULL;

  g_strstrip (name);
  g_strstrip (version);

  /* Only vanilla exists until the loaders are added; the combo row has
   * a single entry, so the selection cannot be anything else. */
  instance = aml_instance_manager_create (self->manager, name, self->directory,
                                          version, AML_LOADER_VANILLA, NULL,
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
  AmlCreateInstanceDialog *self = user_data;
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
        return;

      show_error (self, error->message);
      return;
    }

  g_free (self->directory);
  self->directory = g_file_get_path (file);

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->directory_row),
                               self->directory);
}

static void
on_pick_directory (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlCreateInstanceDialog *self = user_data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();

  gtk_file_dialog_select_folder (dialog,
                                 GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                                 NULL, on_folder_selected, self);
}

static void
aml_create_instance_dialog_init (AmlCreateInstanceDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), "Create Instance");

  self->name_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name_row), "Name");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (self->name_row), TRUE);

  /* Free text until Mojang's version metadata exists: phase 6 replaces
   * this row with a list of the real versions. */
  self->version_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->version_row),
                                 "Minecraft Version");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (self->version_row), TRUE);

  const char *loaders[] = { "Vanilla", NULL };
  GtkWidget *loader_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (loader_row), "Loader");
  adw_combo_row_set_model (ADW_COMBO_ROW (loader_row),
                           G_LIST_MODEL (gtk_string_list_new (loaders)));

  /* What AmlInstanceManager derives when no directory is picked:
   * <data dir>/instances. */
  g_autofree char *default_directory =
    g_build_filename (g_get_user_data_dir (), "AML", "instances", NULL);

  self->directory_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->directory_row),
                                 "Directory");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->directory_row),
                               default_directory);

  GtkWidget *pick_button = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_add_css_class (pick_button, "flat");
  gtk_widget_set_valign (pick_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (pick_button, "Choose a folder");
  g_signal_connect (pick_button, "clicked", G_CALLBACK (on_pick_directory), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row), pick_button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->directory_row),
                                         pick_button);

  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->name_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->version_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), loader_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->directory_row);

  GtkWidget *page = adw_preferences_page_new ();
  gtk_widget_set_vexpand (page, TRUE);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (group));

  self->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->error_label), 0.0);
  gtk_widget_set_margin_start (self->error_label, 24);
  gtk_widget_set_margin_end (self->error_label, 24);
  gtk_widget_set_visible (self->error_label, FALSE);

  GtkWidget *cancel_button = gtk_button_new_with_label ("Cancel");
  g_signal_connect (cancel_button, "clicked", G_CALLBACK (on_cancel_clicked),
                    self);

  self->create_button = gtk_button_new_with_label ("Create");
  gtk_widget_add_css_class (self->create_button, "suggested-action");
  g_signal_connect (self->create_button, "clicked", G_CALLBACK (on_create_clicked),
                    self);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_start (buttons, 24);
  gtk_widget_set_margin_end (buttons, 24);
  gtk_widget_set_margin_bottom (buttons, 24);
  gtk_box_append (GTK_BOX (buttons), cancel_button);
  gtk_box_append (GTK_BOX (buttons), self->create_button);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), page);
  gtk_box_append (GTK_BOX (content), self->error_label);
  gtk_box_append (GTK_BOX (content), buttons);
  adw_dialog_set_child (ADW_DIALOG (self), content);

  adw_dialog_set_default_widget (ADW_DIALOG (self), self->create_button);

  g_signal_connect (self->name_row, "notify::text", G_CALLBACK (on_input_changed),
                    self);
  g_signal_connect (self->version_row, "notify::text",
                    G_CALLBACK (on_input_changed), self);

  update_create_button (self);
}

AmlCreateInstanceDialog *
aml_create_instance_dialog_new (AmlInstanceManager *manager)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (manager), NULL);

  AmlCreateInstanceDialog *self =
    g_object_new (AML_TYPE_CREATE_INSTANCE_DIALOG, NULL);

  self->manager = g_object_ref (manager);

  return self;
}

static void
aml_create_instance_dialog_dispose (GObject *object)
{
  AmlCreateInstanceDialog *self = AML_CREATE_INSTANCE_DIALOG (object);

  g_clear_object (&self->manager);

  G_OBJECT_CLASS (aml_create_instance_dialog_parent_class)->dispose (object);
}

static void
aml_create_instance_dialog_finalize (GObject *object)
{
  AmlCreateInstanceDialog *self = AML_CREATE_INSTANCE_DIALOG (object);

  g_free (self->directory);

  G_OBJECT_CLASS (aml_create_instance_dialog_parent_class)->finalize (object);
}

static void
aml_create_instance_dialog_class_init (AmlCreateInstanceDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_create_instance_dialog_dispose;
  object_class->finalize = aml_create_instance_dialog_finalize;
}
