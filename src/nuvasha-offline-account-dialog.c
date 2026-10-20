/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-offline-account-dialog.h"
#include "nuvasha-i18n.h"
#include "nuvasha-ui-feedback.h"

struct _NuvashaOfflineAccountDialog
{
  AdwDialog parent_instance;

  AdwEntryRow *name_row;
  GtkLabel *error_label;
  GtkButton *add_button;
};

G_DEFINE_FINAL_TYPE (NuvashaOfflineAccountDialog, nuvasha_offline_account_dialog,
                     ADW_TYPE_DIALOG)

static guint
  nuvasha_offline_account_dialog_signals[nuvasha_OFFLINE_ACCOUNT_DIALOG_N_SIGNALS];

static void
show_error (NuvashaOfflineAccountDialog *self, const char *message)
{
  gtk_label_set_text (self->error_label, message);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), TRUE);
}

/* The Add button is sensitive when the name is one the game accepts, and
 * the reason it is not is written under the entry: which of the two it is
 * comes from the same check the store makes, so the dialog cannot accept
 * what the store would refuse. */
static void
update_add_button (NuvashaOfflineAccountDialog *self)
{
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaAccount) account = NULL;

  g_strstrip (name);
  account = nuvasha_account_new_offline (name, &error);

  gtk_widget_set_sensitive (GTK_WIDGET (self->add_button), account != NULL);

  /* An empty entry is not a mistake, it is one no one has typed in yet:
     only a name that was really typed and refused gets a sentence. */
  if (account != NULL || name[0] == '\0')
    nuvasha_ui_clear_error_label (self->error_label);
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
  NuvashaOfflineAccountDialog *self = nuvasha_OFFLINE_ACCOUNT_DIALOG (user_data);
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaAccount) account = NULL;

  g_strstrip (name);
  account = nuvasha_account_new_offline (name, &error);

  /* The button is insensitive when this fails, so this only guards
     against the entry being emptied underneath it. */
  if (account == NULL)
    {
      show_error (self, error->message);
      return;
    }

  g_signal_emit (self,
                 nuvasha_offline_account_dialog_signals[nuvasha_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED],
                 0, account);
}

static void
nuvasha_offline_account_dialog_init (NuvashaOfflineAccountDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), _("Add Offline Account"));

  self->name_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name_row),
                                 "Name");
  adw_entry_row_set_max_length (ADW_ENTRY_ROW (self->name_row), 16);
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (self->name_row), TRUE);

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_add (group,
                             GTK_WIDGET (self->name_row));

  self->error_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->error_label), 0.0);
  gtk_widget_add_css_class (GTK_WIDGET (self->error_label), "error");
  gtk_widget_set_valign (GTK_WIDGET (self->error_label), GTK_ALIGN_START);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);

  GtkButton *cancel_button = GTK_BUTTON (gtk_button_new_with_label (_("Cancel")));
  g_signal_connect (cancel_button, "clicked", G_CALLBACK (nuvasha_ui_cancel_dialog),
                    self);

  self->add_button = GTK_BUTTON (gtk_button_new_with_label (_("Add")));
  gtk_widget_add_css_class (GTK_WIDGET (self->add_button), "suggested-action");
  g_signal_connect (self->add_button, "clicked", G_CALLBACK (on_add_clicked),
                    self);

  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (cancel_button));
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (self->add_button));

  /* Keep this form like the export-metadata form: a natural vertical box
     containing the field group, the validation message, and the buttons.
     A preferences page here adds a scrolling allocation around the entry
     and can let the message paint over the row. */
  GtkBox *content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  gtk_widget_set_margin_start (GTK_WIDGET (content), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (content), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (content), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (content), 12);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (group));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->error_label));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (buttons));
  adw_dialog_set_child (ADW_DIALOG (self), GTK_WIDGET (content));

  adw_dialog_set_default_widget (ADW_DIALOG (self),
                                 GTK_WIDGET (self->add_button));

  g_signal_connect (self->name_row, "notify::text",
                    G_CALLBACK (on_input_changed), self);

  update_add_button (self);
}

static void
nuvasha_offline_account_dialog_class_init (NuvashaOfflineAccountDialogClass *klass)
{
  nuvasha_offline_account_dialog_signals[nuvasha_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED] =
    g_signal_new ("account-added", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                  nuvasha_TYPE_ACCOUNT);
}

NuvashaOfflineAccountDialog *
nuvasha_offline_account_dialog_new (void)
{
  return g_object_new (nuvasha_TYPE_OFFLINE_ACCOUNT_DIALOG, NULL);
}
