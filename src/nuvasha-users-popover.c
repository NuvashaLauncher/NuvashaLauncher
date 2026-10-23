/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-users-popover.h"
#include "nuvasha-i18n.h"

#include "nuvasha-offline-account-dialog.h"
#include "nuvasha-sign-in-dialog.h"
#include "nuvasha-skin-view.h"
#include "nuvasha-fs.h"
#include "nuvasha-ui-feedback.h"
#include "nuvasha-users-popover-private.h"

#include <libsoup/soup.h>

/* How wide the list is: enough for a name and its kind to be read, and no
 * wider than the header the button of it sits in. */
#define USERS_WIDTH 420
#define USERS_ROWS_WIDTH 396

/* A skin is a picture of 64 by 64 and the face is 8 by 8 of it, in the
 * middle row. What is shown here is that face, at the size a row is
 * tall. */
#define AVATAR_SIZE 40
/* The header button shows the avatar alone, at the size of the other
 * header icons rather than a row's. */
#define HEADER_AVATAR_SIZE 16


G_DEFINE_FINAL_TYPE (NuvashaUsersPopover, nuvasha_users_popover, GTK_TYPE_POPOVER)

/* Update the header button for the active account. */
static void
update_the_button (NuvashaUsersPopover *self)
{
  NuvashaAccount *account = nuvasha_accounts_get_active (self->accounts);

  g_clear_pointer (&self->active_name, g_free);
  nuvasha_users_set_avatar_icon (GTK_IMAGE (self->avatar_image));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self), NULL);

  if (account == NULL)
    {
      self->active_name = g_strdup ("");
      return;
    }

  self->active_name = g_strdup (account->name);
  nuvasha_users_set_avatar_icon (GTK_IMAGE (self->avatar_image));

}


/* ------------------------------------------------------ *
 * One account
 * ------------------------------------------------------ */

static void
show_error (NuvashaUsersPopover *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), message);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), TRUE);
}

/* The account a widget in a row is about, which is the id of the account
   the row was built from. */
static const char *
account_id_of (gpointer widget)
{
  return g_object_get_data (G_OBJECT (widget), "nuvasha-account-id");
}

AdwDialog *nuvasha_users_skin_menu_new (NuvashaUsersPopover *owner, GtkImage *image,
                                 NuvashaAccount *account);

static void
open_skin_menu_for_account (NuvashaUsersPopover *self, const char *id,
                            GtkImage *image)
{
  NuvashaAccount *account = nuvasha_accounts_find (self->accounts, id);
  AdwDialog *dialog;

  if (account == NULL || image == NULL)
    return;
  gtk_popover_popdown (GTK_POPOVER (self));
  dialog = nuvasha_users_skin_menu_new (self, image, account);
  gtk_widget_set_focusable (GTK_WIDGET (dialog), TRUE);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
  gtk_widget_grab_focus (GTK_WIDGET (dialog));
}

static void
on_avatar_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = account_id_of (button);
  GtkImage *image = GTK_IMAGE (gtk_button_get_child (button));

  open_skin_menu_for_account (self, id, image);
}

static void
on_row_activated (AdwActionRow *row, gpointer user_data G_GNUC_UNUSED)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = account_id_of (row);
  g_autoptr (GError) error = NULL;

  if (id != NULL && !nuvasha_accounts_set_active (self->accounts, id, &error))
    show_error (self, error->message);
  else
    gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_remove_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = g_object_get_data (G_OBJECT (source), "nuvasha-account-id");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (id != NULL && g_strcmp0 (response, "remove") == 0)
    {
      g_autoptr (GError) error = NULL;

      if (!nuvasha_accounts_remove (self->accounts, id, &error))
        show_error (self, error->message);
    }

  g_object_unref (self);
}

/* Removing an account throws away its saved sign-in for good, so it is
 * asked for first. */
static void
on_remove_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = account_id_of (button);
  const char *name = g_object_get_data (G_OBJECT (button), "nuvasha-account-name");
  g_autofree char *heading = NULL;
  AdwAlertDialog *alert = NULL;

  if (id == NULL)
    return;

  /* Move focus away from the popover before showing the confirmation. */
  gtk_popover_popdown (GTK_POPOVER (self));

  heading = g_strdup_printf (
    _("Remove %s?"), name != NULL && *name != '\0' ? name : _("this account"));

  alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (
      heading, _("The account is removed from Nuvasha and its saved sign-in is "
                 "forgotten. The game itself is not touched.")));
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "remove", _("Remove"));
  adw_alert_dialog_set_response_appearance (alert, "remove",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  g_object_set_data_full (G_OBJECT (alert), "nuvasha-account-id",
                          g_strdup (id), g_free);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_remove_chosen,
                           g_object_ref (self));
}

/* One account as a row of the list. The whole row opens the skin settings
 * for the account it is about, rather than requiring a click on the face.
 * The account that is active carries a check mark, and the trash button
 * takes an account out. */
static void
append_account_row (NuvashaUsersPopover *self, const NuvashaAccount *account)
{
  gboolean is_active =
    g_str_equal (nuvasha_accounts_get_active_id (self->accounts) != NULL
                   ? nuvasha_accounts_get_active_id (self->accounts)
                   : "",
                 account->id);
  gboolean needs_sign_in =
    account->type == nuvasha_ACCOUNT_MICROSOFT &&
    !nuvasha_account_token_is_valid (account, g_get_real_time () / 1000000);

  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkImage *image = GTK_IMAGE (gtk_image_new ());
  GtkButton *avatar = GTK_BUTTON (gtk_button_new ());
  GtkButton *remove = GTK_BUTTON (gtk_button_new_from_icon_name ("user-trash-symbolic"));
  g_autofree char *tooltip = NULL;

  /* The name and the kind are plain text: an account name may hold an
     "&". Owned by the row, which is itself the clickable target. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), account->name);

  /* The kind of account is on every row: an offline session and a
     Microsoft one are not the same thing to play with, and the user is
     the one who has to know which one a launch will run as. An account
     whose token has run out says so here, which is earlier than finding
     it out from a launch that stops. */
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                                needs_sign_in ? _("Microsoft · Needs sign-in")
                                             : nuvasha_account_type_name (
                                                   account->type));

  if (needs_sign_in)
    gtk_widget_add_css_class (GTK_WIDGET (row), "warning");

  nuvasha_users_set_avatar_icon (GTK_IMAGE (image));
  if (account->type == nuvasha_ACCOUNT_OFFLINE)
    {
      if (account->avatar_url != NULL && *account->avatar_url != '\0')
        nuvasha_users_set_avatar (self, GTK_IMAGE (image), account->avatar_url);
      else
        {
          g_autofree char *steve = nuvasha_users_default_steve_path ();
          if (!nuvasha_users_set_local_avatar (GTK_IMAGE (image), steve))
            nuvasha_users_set_avatar_icon (GTK_IMAGE (image));
        }
    }
  else
    nuvasha_users_set_avatar (self, GTK_IMAGE (image), account->avatar_url);
  gtk_image_set_pixel_size (GTK_IMAGE (image), AVATAR_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (image), GTK_ALIGN_CENTER);
  gtk_button_set_child (avatar, GTK_WIDGET (image));
  gtk_button_set_has_frame (avatar, FALSE);
  gtk_widget_set_valign (GTK_WIDGET (avatar), GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (avatar), "nuvasha-account-id",
                          g_strdup (account->id), g_free);
  g_signal_connect_object (avatar, "clicked", G_CALLBACK (on_avatar_clicked), self, 0);
  g_object_set_data (G_OBJECT (row), "nuvasha-account-image", image);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row),
                             GTK_WIDGET (avatar));

  /* The mark of the one a launch plays as, at the far end of the row. */
  if (is_active)
    {
      GtkImage *check = GTK_IMAGE (gtk_image_new_from_icon_name ("object-select-symbolic"));

      gtk_widget_add_css_class (GTK_WIDGET (check), "accent");
      gtk_widget_set_valign (GTK_WIDGET (check), GTK_ALIGN_CENTER);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (check));
    }

  tooltip = g_strdup_printf (_("Remove \"%s\""), account->name);
  nuvasha_ui_prepare_icon_button (GTK_WIDGET (remove), tooltip);
  gtk_widget_set_valign (GTK_WIDGET (remove), GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (remove), "nuvasha-account-id",
                          g_strdup (account->id), g_free);
  g_object_set_data_full (G_OBJECT (remove), "nuvasha-account-name",
                          g_strdup (account->name), g_free);
  g_signal_connect_object (remove, "clicked", G_CALLBACK (on_remove_clicked), self, 0);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), GTK_WIDGET (remove));

  /* Which account a row opens travels with the row: the list is built again
     whenever the store changes, and a row is never asked about anything
     else. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data_full (G_OBJECT (row), "nuvasha-account-id",
                          g_strdup (account->id), g_free);
  g_signal_connect_object (row, "activated", G_CALLBACK (on_row_activated), self, 0);

  gtk_list_box_append (GTK_LIST_BOX (self->list_box), GTK_WIDGET (row));
}


/* ------------------------------------------------------ *
 * Adding one
 * ------------------------------------------------------ */

/* An account the user just signed in to, or typed a name for, goes into
   the store, which is what makes it the active one and writes it down. */
static void
add_account (NuvashaUsersPopover *self, NuvashaAccount *account)
{
  g_autoptr (GError) error = NULL;

  if (!nuvasha_accounts_add (self->accounts, account, &error))
    show_error (self, error->message);
  else
    nuvasha_ui_clear_error_label (self->error_label);
}

static void
on_account_added (AdwDialog *dialog, NuvashaAccount *account, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);

  add_account (self, account);
  adw_dialog_close (ADW_DIALOG (dialog));
  gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_add_microsoft_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  NuvashaSignInDialog *dialog = nuvasha_sign_in_dialog_new ();

  /* Weak, so that the handler does not keep the dialog alive for as long
     as it is connected to it. */
  g_signal_connect_object (dialog, "signed-in", G_CALLBACK (on_account_added),
                           self, 0);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
  gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_add_offline_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  NuvashaOfflineAccountDialog *dialog = nuvasha_offline_account_dialog_new ();

  g_signal_connect_object (dialog, "account-added",
                            G_CALLBACK (on_account_added), self, 0);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
  gtk_popover_popdown (GTK_POPOVER (self));
}


/* ------------------------------------------------------ *
 * The whole list
 * ------------------------------------------------------ */

/* What the store says, from scratch: every account in the order they were
   added, with the active one marked, and a sentence in place of the list
   when there are none. */
static void
rebuild (NuvashaUsersPopover *self)
{
  GPtrArray *accounts = nuvasha_accounts_list (self->accounts);
  GtkWidget *child;

  /* Whatever was still on its way belongs to rows that are about to be
     gone. */
  g_cancellable_cancel (self->avatars);
  g_clear_object (&self->avatars);
  self->avatars = g_cancellable_new ();

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->list_box))) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  /* With no accounts the list is not there at all: an empty boxed list
     still draws a blank card, and the scroller around it leaves a gap
     above the two buttons that add one. */
  gtk_widget_set_visible (GTK_WIDGET (self->list_scroller), accounts->len > 0);

  for (guint i = 0; i < accounts->len; i++)
    append_account_row (self, g_ptr_array_index (accounts, i));

  update_the_button (self);
}

static void
on_accounts_changed (NuvashaAccounts *accounts G_GNUC_UNUSED,
                     gpointer    user_data)
{
  rebuild (user_data);
}

static void
nuvasha_users_popover_init (NuvashaUsersPopover *self)
{
  GtkScrolledWindow *scrolled = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  GtkListBox *rows = GTK_LIST_BOX (gtk_list_box_new ());
  GtkButton *microsoft = GTK_BUTTON (gtk_button_new_with_label (_("Add Microsoft Account")));
  GtkButton *offline = GTK_BUTTON (gtk_button_new_with_label (_("Add Offline Account")));

  /* A row is chosen by clicking it, not by a selection the list keeps:
     the active account is the store's business. The list scrolls instead
     of the popover growing without end when there are many accounts. */
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (rows), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (rows), "boxed-list");
  gtk_widget_add_css_class (GTK_WIDGET (rows), "nuvasha-list-section");
  gtk_widget_set_size_request (GTK_WIDGET (scrolled), USERS_WIDTH, -1);
  gtk_scrolled_window_set_propagate_natural_width (
      GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (scrolled), GTK_ALIGN_CENTER);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), GTK_WIDGET (rows));
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled),
                                              420);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_widget_set_size_request (GTK_WIDGET (rows), USERS_ROWS_WIDTH, -1);

  self->list_box = GTK_LIST_BOX (rows);
  self->list_scroller = GTK_SCROLLED_WINDOW (scrolled);

  self->error_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->error_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->error_label), "error");
  gtk_widget_set_margin_start (GTK_WIDGET (self->error_label), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->error_label), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->error_label), 6);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);

  /* The two ways of having an account. The first one signs in without a
     password: the user types a code into a page of Microsoft's own. */
  gtk_widget_add_css_class (GTK_WIDGET (microsoft), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (offline), "flat");
  gtk_widget_set_halign (GTK_WIDGET (microsoft), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (offline), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (microsoft), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (offline), TRUE);
  gtk_widget_set_margin_start (GTK_WIDGET (microsoft), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (microsoft), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (offline), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (offline), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (microsoft), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (offline), 12);
  g_signal_connect_object (microsoft, "clicked",
                           G_CALLBACK (on_add_microsoft_clicked), self, 0);
  g_signal_connect_object (offline, "clicked", G_CALLBACK (on_add_offline_clicked),
                           self, 0);

  GtkBox *content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (scrolled));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (microsoft));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (offline));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->error_label));

  gtk_popover_set_child (GTK_POPOVER (self), GTK_WIDGET (content));

  /* What the header says about who is playing: the avatar of the active
     account, or the default one when there is none. It is filled from the
     same store as the list, so the two cannot disagree. */
  self->avatar_image = GTK_IMAGE (gtk_image_new_from_icon_name ("avatar-default-symbolic"));
  gtk_image_set_pixel_size (GTK_IMAGE (self->avatar_image),
                            HEADER_AVATAR_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (self->avatar_image), GTK_ALIGN_CENTER);

  self->button_content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  g_object_ref_sink (self->button_content);
  gtk_box_append (GTK_BOX (self->button_content), GTK_WIDGET (self->avatar_image));
}

static void
nuvasha_users_popover_constructed (GObject *object)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  /* Weak, so that the handler does not keep the store alive for as long
     as the popover is connected to it. */
  g_signal_connect_object (self->accounts, "changed",
                           G_CALLBACK (on_accounts_changed), self, 0);

  G_OBJECT_CLASS (nuvasha_users_popover_parent_class)->constructed (object);

  self->avatars = g_cancellable_new ();
  self->avatar_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              g_object_unref);
  rebuild (self);
}

static void
nuvasha_users_popover_dispose (GObject *object)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  /* An avatar that is still on its way is not needed by anything any
     more. */
  g_clear_object (&self->avatars);

  G_OBJECT_CLASS (nuvasha_users_popover_parent_class)->dispose (object);
}

static void
nuvasha_users_popover_finalize (GObject *object)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  g_clear_object (&self->accounts);
  g_clear_object (&self->button_content);
  g_clear_pointer (&self->avatar_cache, g_hash_table_unref);
  g_free (self->active_name);

  G_OBJECT_CLASS (nuvasha_users_popover_parent_class)->finalize (object);
}

enum {
  PROP_ACCOUNTS = 1,
  PROP_LAST
};

static void
nuvasha_users_popover_set_accounts (NuvashaUsersPopover *self, NuvashaAccounts *accounts);

static void
nuvasha_users_popover_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  switch (property_id)
    {
    case PROP_ACCOUNTS:
      g_value_set_object (value, self->accounts);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
nuvasha_users_popover_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  switch (property_id)
    {
    case PROP_ACCOUNTS:
      nuvasha_users_popover_set_accounts (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
nuvasha_users_popover_class_init (NuvashaUsersPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = nuvasha_users_popover_constructed;
  object_class->dispose = nuvasha_users_popover_dispose;
  object_class->finalize = nuvasha_users_popover_finalize;
  object_class->get_property = nuvasha_users_popover_get_property;
  object_class->set_property = nuvasha_users_popover_set_property;

  /* The store has to be there before the popover is built, because what
     is built is what the store holds. */
  g_object_class_install_property (
      object_class, PROP_ACCOUNTS,
      g_param_spec_object ("accounts", NULL, NULL, nuvasha_TYPE_ACCOUNTS,
                           G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
}

/* The store is taken by reference and never let go of before the
   popover is: it is what the whole widget is built from. */
static void
nuvasha_users_popover_set_accounts (NuvashaUsersPopover *self, NuvashaAccounts *accounts)
{
  g_set_object (&self->accounts, accounts);
}

NuvashaUsersPopover *
nuvasha_users_popover_new (NuvashaAccounts *accounts)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (accounts), NULL);

  return g_object_new (nuvasha_TYPE_USERS_POPOVER, "accounts", accounts, NULL);
}

NuvashaAccounts *
nuvasha_users_popover_get_accounts (NuvashaUsersPopover *self)
{
  g_return_val_if_fail (nuvasha_IS_USERS_POPOVER (self), NULL);

  return self->accounts;
}

char *
nuvasha_users_popover_get_active_name (NuvashaUsersPopover *self)
{
  g_return_val_if_fail (nuvasha_IS_USERS_POPOVER (self), NULL);

  return g_strdup (self->active_name);
}

/* The avatar and the name in the header, which is what the menu button of
 * the popover shows. It is built here rather than in the init because it
 * belongs to the popover and is handed over by the window. */
GtkWidget *
nuvasha_users_popover_get_button_content (NuvashaUsersPopover *self)
{
  g_return_val_if_fail (nuvasha_IS_USERS_POPOVER (self), NULL);

  /* The same widget every time: a caller that asked twice is holding two
     references to one button, which is what it would want, and never two
     buttons over one avatar. */
  return GTK_WIDGET (g_object_ref (self->button_content));
}
