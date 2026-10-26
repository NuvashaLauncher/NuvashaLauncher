/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-offline-account-dialog.h"
#include "nuvashalauncher-dialogs.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-ui-feedback.h"

struct _nuvashalauncherOfflineAccountDialog
{
  AdwDialog parent_instance;

  AdwEntryRow *name_row;
  GtkLabel *error_label;
  GtkButton *add_button;
  GtkButton *cancel_button;
  GtkBox *content;
  nuvashalauncherAccounts *accounts;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherOfflineAccountDialog, nuvashalauncher_offline_account_dialog,
                     ADW_TYPE_DIALOG)

static guint
  nuvashalauncher_offline_account_dialog_signals[nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_N_SIGNALS];

static void
show_error (nuvashalauncherOfflineAccountDialog *self, const char *message)
{
  (void) message;
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);
}

static gboolean
offline_name_exists (nuvashalauncherOfflineAccountDialog *self, const char *name)
{
  if (self->accounts == NULL)
    return FALSE;

  GPtrArray *accounts = nuvashalauncher_accounts_list (self->accounts);

  for (guint i = 0; i < accounts->len; i++)
    {
      nuvashalauncherAccount *existing = g_ptr_array_index (accounts, i);

      if (existing->type == nuvashalauncher_ACCOUNT_OFFLINE &&
          g_strcmp0 (existing->name, name) == 0)
        return TRUE;
    }

  return FALSE;
}

/* The Add button is sensitive when the name is one the game accepts, and
 * the reason it is not is written under the entry: which of the two it is
 * comes from the same check the store makes, so the dialog cannot accept
 * what the store would refuse. */
static void
update_add_button (nuvashalauncherOfflineAccountDialog *self)
{
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherAccount) account = NULL;

  g_strstrip (name);
  account = nuvashalauncher_account_new_offline (name, &error);

  if (account != NULL && offline_name_exists (self, name))
    g_clear_pointer (&account, nuvashalauncher_account_free);

  gtk_widget_set_sensitive (GTK_WIDGET (self->add_button), account != NULL);

  /* An empty entry is not a mistake, it is one no one has typed in yet:
     only a name that was really typed and refused gets a sentence. */
  if (account != NULL || name[0] == '\0')
    nuvashalauncher_ui_clear_error_label (self->error_label);
  else
    nuvashalauncher_ui_clear_error_label (self->error_label);
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
  nuvashalauncherOfflineAccountDialog *self = nuvashalauncher_OFFLINE_ACCOUNT_DIALOG (user_data);
  g_autofree char *name =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name_row)));
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherAccount) account = NULL;

  g_strstrip (name);
  account = nuvashalauncher_account_new_offline (name, &error);

  /* The button is insensitive when this fails, so this only guards
     against the entry being emptied underneath it. */
  if (account == NULL || offline_name_exists (self, name))
    {
      if (account == NULL)
        show_error (self, error->message);
      return;
    }

  g_signal_emit (self,
                 nuvashalauncher_offline_account_dialog_signals[nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED],
                 0, account);
}

static void
nuvashalauncher_offline_account_dialog_init (nuvashalauncherOfflineAccountDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
  /* Keep a nonzero baseline for Adwaita's surface sizing, but let the
   * header and form determine the actual height. */
  gtk_widget_set_size_request (GTK_WIDGET (self), 360, 1);
  adw_dialog_set_follows_content_size (ADW_DIALOG (self), TRUE);
  nuvashalauncher_dialog_add_content_margin_breakpoint (ADW_DIALOG (self),
                                                GTK_WIDGET (self->content),
                                                440, 6);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name_row), _("Name"));
  adw_entry_row_set_max_length (self->name_row, 16);
  adw_entry_row_set_activates_default (self->name_row, TRUE);
  gtk_label_set_wrap (self->error_label, TRUE);
  gtk_label_set_xalign (self->error_label, 0.0);
  gtk_widget_add_css_class (GTK_WIDGET (self->error_label), "error");
  gtk_widget_set_valign (GTK_WIDGET (self->error_label), GTK_ALIGN_START);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);
  gtk_button_set_label (self->cancel_button, _("Cancel"));
  gtk_button_set_label (self->add_button, _("Add"));
  gtk_widget_add_css_class (GTK_WIDGET (self->add_button), "suggested-action");
  g_signal_connect_object (self->cancel_button, "clicked", G_CALLBACK (nuvashalauncher_ui_cancel_dialog), self, 0);
  g_signal_connect_object (self->add_button, "clicked", G_CALLBACK (on_add_clicked), self, 0);
  g_signal_connect_object (self->name_row, "notify::text", G_CALLBACK (on_input_changed), self, 0);
  adw_dialog_set_default_widget (ADW_DIALOG (self), GTK_WIDGET (self->add_button));
  update_add_button (self);
}

static void
nuvashalauncher_offline_account_dialog_class_init (nuvashalauncherOfflineAccountDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  gtk_widget_class_set_template_from_resource (widget_class,
      "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-offline-account.ui");
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherOfflineAccountDialog, name_row);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherOfflineAccountDialog, error_label);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherOfflineAccountDialog, add_button);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherOfflineAccountDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherOfflineAccountDialog, content);
  nuvashalauncher_offline_account_dialog_signals[nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED] =
    g_signal_new ("account-added", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                  nuvashalauncher_TYPE_ACCOUNT);
}

nuvashalauncherOfflineAccountDialog *
nuvashalauncher_offline_account_dialog_new (void)
{
  return g_object_new (nuvashalauncher_TYPE_OFFLINE_ACCOUNT_DIALOG, NULL);
}

void
nuvashalauncher_offline_account_dialog_set_accounts (nuvashalauncherOfflineAccountDialog *self,
                                             nuvashalauncherAccounts *accounts)
{
  g_return_if_fail (nuvashalauncher_IS_OFFLINE_ACCOUNT_DIALOG (self));
  g_return_if_fail (accounts == NULL || nuvashalauncher_IS_ACCOUNTS (accounts));

  self->accounts = accounts;
  update_add_button (self);
}
