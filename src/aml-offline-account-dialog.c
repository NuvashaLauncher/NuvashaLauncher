/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-offline-account-dialog.h"

struct _AmlOfflineAccountDialog
{
  AdwDialog parent_instance;

  GtkWidget *name_row;
  GtkWidget *error_label;
  GtkWidget *add_button;
};

G_DEFINE_FINAL_TYPE (AmlOfflineAccountDialog, aml_offline_account_dialog,
                     ADW_TYPE_DIALOG)

static guint
  aml_offline_account_dialog_signals[AML_OFFLINE_ACCOUNT_DIALOG_N_SIGNALS];

static void
show_error (AmlOfflineAccountDialog *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), message);
  gtk_widget_set_visible (self->error_label, TRUE);
}

static void
clear_error (AmlOfflineAccountDialog *self)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), NULL);
  gtk_widget_set_visible (self->error_label, FALSE);
}

/* The Add button is sensitive when the name is one the game accepts, and
 * the reason it is not is written under the entry: which of the two it is
 * comes from the same check the store makes, so the dialog cannot accept
 * what the store would refuse. */
static void
update_add_button (AmlOfflineAccountDialog *self)
{
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlAccount) account = NULL;

  g_strstrip (name);
  account = aml_account_new_offline (name, &error);

  gtk_widget_set_sensitive (self->add_button, account != NULL);

  /* An empty entry is not a mistake, it is one no one has typed in yet:
     only a name that was really typed and refused gets a sentence. */
  if (account != NULL || name[0] == '\0')
    clear_error (self);
  else
    show_error (self, error->message);
}

static void
on_input_changed (GObject *object G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED,
                  gpointer user_data)
{
  update_add_button (user_data);
}

static void
on_add_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlOfflineAccountDialog *self = user_data;
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autoptr (GError) error = NULL;
  AmlAccount *account = NULL;

  g_strstrip (name);
  account = aml_account_new_offline (name, &error);

  /* The button is insensitive when this fails, so this only guards
     against the entry being emptied underneath it. */
  if (account == NULL)
    {
      show_error (self, error->message);
      return;
    }

  g_signal_emit (self,
                 aml_offline_account_dialog_signals[AML_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED],
                 0, account);
}

static void
on_cancel_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  adw_dialog_close (ADW_DIALOG (user_data));
}

static void
aml_offline_account_dialog_init (AmlOfflineAccountDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), "Add Offline Account");

  self->name_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name_row),
                                 "Name");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (self->name_row), TRUE);

  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->name_row);

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

  self->add_button = gtk_button_new_with_label ("Add");
  gtk_widget_add_css_class (self->add_button, "suggested-action");
  g_signal_connect (self->add_button, "clicked", G_CALLBACK (on_add_clicked),
                    self);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_start (buttons, 24);
  gtk_widget_set_margin_end (buttons, 24);
  gtk_widget_set_margin_bottom (buttons, 24);
  gtk_box_append (GTK_BOX (buttons), cancel_button);
  gtk_box_append (GTK_BOX (buttons), self->add_button);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), page);
  gtk_box_append (GTK_BOX (content), self->error_label);
  gtk_box_append (GTK_BOX (content), buttons);
  adw_dialog_set_child (ADW_DIALOG (self), content);

  adw_dialog_set_default_widget (ADW_DIALOG (self), self->add_button);

  g_signal_connect (self->name_row, "notify::text",
                    G_CALLBACK (on_input_changed), self);

  update_add_button (self);
}

static void
aml_offline_account_dialog_class_init (AmlOfflineAccountDialogClass *klass)
{
  aml_offline_account_dialog_signals[AML_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED] =
    g_signal_new ("account-added", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                  AML_TYPE_ACCOUNT);
}

AmlOfflineAccountDialog *
aml_offline_account_dialog_new (void)
{
  return g_object_new (AML_TYPE_OFFLINE_ACCOUNT_DIALOG, NULL);
}
