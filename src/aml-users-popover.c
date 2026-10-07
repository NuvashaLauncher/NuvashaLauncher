/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-users-popover.h"

#include "aml-offline-account-dialog.h"
#include "aml-sign-in-dialog.h"

/* How wide the list is: enough for a name and its kind to be read, and no
 * wider than the header the button of it sits in. */
#define USERS_WIDTH 360
#define USERS_ROWS_WIDTH 336

/* A skin is a picture of 64 by 64 and the face is 8 by 8 of it, in the
 * middle row. What is shown here is that face, at the size a row is
 * tall. */
#define AVATAR_SIZE 40
/* The header button shows the avatar alone, at the size of the other
 * header icons rather than a row's. */
#define HEADER_AVATAR_SIZE 16

struct _AmlUsersPopover
{
  GtkPopover parent_instance;

  AmlAccounts *accounts;

  GtkWidget *list_box;    /* the accounts, one row each */
  GtkWidget *empty_label; /* what is said when there are none */
  GtkWidget *error_label; /* what went wrong, when something did */
  GtkWidget *avatar_image;
  GtkWidget *button_content; /* what the header button shows */

  /* Avatars are pictures of the internet: they are fetched after the row
     that wants one is on screen, and a fetch that is still running when
     the rows are built again is stopped with this. */
  GCancellable *avatars;

  char *active_name;
};

G_DEFINE_FINAL_TYPE (AmlUsersPopover, aml_users_popover, GTK_TYPE_POPOVER)

/* A skin is a picture of 64 by 64 and the face of it is 8 by 8 of that,
 * in the middle of the top of the picture. */
#define SKIN_SIZE 64
#define FACE_SIZE 8
#define FACE_X 8
#define FACE_Y 8

/* The face out of a skin, as a texture the image widget can show.
 *
 * The picture is read into memory, the face of it is copied out of the
 * middle of the top, and every pixel of that face is grown to the size
 * it is shown at: a skin is drawn in whole pixels and there is nothing
 * between them that would be worth smoothing. */
static GdkTexture *
face_texture (GdkTexture *skin)
{
  gsize skin_stride = 0;
  gsize face_stride = (gsize) AVATAR_SIZE * 4;
  g_autofree guchar *pixels = NULL;
  g_autofree guchar *face = NULL;

  if (skin == NULL || gdk_texture_get_width (skin) != SKIN_SIZE ||
      gdk_texture_get_height (skin) != SKIN_SIZE)
    return NULL;

  skin_stride = (gsize) SKIN_SIZE * 4;
  pixels = g_malloc (skin_stride * SKIN_SIZE);

  /* What comes back is four bytes to a pixel, blue first, which is a
     format a texture of its own is made of. */
  gdk_texture_download (skin, pixels, skin_stride);

  face = g_malloc0 (face_stride * AVATAR_SIZE);

  for (int y = 0; y < AVATAR_SIZE; y++)
    {
      for (int x = 0; x < AVATAR_SIZE; x++)
        {
          int from_x = FACE_X + x * FACE_SIZE / AVATAR_SIZE;
          int from_y = FACE_Y + y * FACE_SIZE / AVATAR_SIZE;
          const guchar *from = pixels + (gsize) from_y * skin_stride +
                               (gsize) from_x * 4;

          memcpy (face + (gsize) y * face_stride + (gsize) x * 4, from, 4);
        }
    }

  return gdk_memory_texture_new (AVATAR_SIZE, AVATAR_SIZE,
                                 GDK_MEMORY_B8G8R8A8,
                                 g_bytes_new_take (g_steal_pointer (&face),
                                                  face_stride * AVATAR_SIZE),
                                 face_stride);
}

static void
on_skin_downloaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  /* The image is held by a reference for as long as the fetch lasts, so
     that a row which is gone by the time the skin arrives is not written
     to. */
  g_autoptr (GtkImage) image = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GdkTexture) skin = NULL;
  g_autoptr (GdkTexture) face = NULL;

  bytes = g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  /* A skin that cannot be fetched or read is no reason to fail anything:
     the icon every account has stands in for it. */
  if (bytes == NULL)
    return;

  skin = gdk_texture_new_from_bytes (bytes, &error);

  if (skin == NULL)
    return;

  face = face_texture (skin);

  if (face != NULL)
    gtk_image_set_from_paintable (image, GDK_PAINTABLE (face));
}

/* Shows the face of @url in @image, or leaves the icon that is already
   there. It never blocks: the picture arrives when it arrives. */
static void
set_avatar (AmlUsersPopover *self,
            GtkImage        *image,
            const char       *url)
{
  g_autoptr (GFile) file = NULL;

  if (url == NULL || *url == '\0')
    return;

  file = g_file_new_for_uri (url);
  g_file_load_bytes_async (file, self->avatars, on_skin_downloaded,
                           g_object_ref (image));
}

/* The icon every account has, and what an offline account has as its
   avatar: there is nothing behind it to have a picture of. */
static void
set_avatar_icon (GtkImage *image)
{
  gtk_image_set_from_icon_name (image, "avatar-default-symbolic");
}

/* What the button in the header says about who this copy of AML plays
   as. An account that has to be signed in again says so there too: a
   name that cannot be used is better known before a launch than after
   one. */
static void
update_the_button (AmlUsersPopover *self)
{
  AmlAccount *account = aml_accounts_get_active (self->accounts);

  g_clear_pointer (&self->active_name, g_free);
  set_avatar_icon (GTK_IMAGE (self->avatar_image));

  if (account == NULL)
    {
      self->active_name = g_strdup ("");

      gtk_widget_set_tooltip_text (GTK_WIDGET (self),
                                   "No account yet: add one to play");
      return;
    }

  self->active_name = g_strdup (account->name);
  set_avatar (self, GTK_IMAGE (self->avatar_image), account->avatar_url);

  {
    gboolean ready = account->type == AML_ACCOUNT_OFFLINE ||
                     aml_account_token_is_valid (account,
                                                 g_get_real_time () / 1000000);
    g_autofree char *tooltip = NULL;

    tooltip = g_strdup_printf ("%s (%s%s)", account->name,
                               aml_account_type_name (account->type),
                               ready ? "" : ", needs sign-in");
    gtk_widget_set_tooltip_text (GTK_WIDGET (self), tooltip);
  }
}

/* ------------------------------------------------------ *
 * One account
 * ------------------------------------------------------ */

static void
show_error (AmlUsersPopover *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), message);
  gtk_widget_set_visible (self->error_label, TRUE);
}

static void
clear_error (AmlUsersPopover *self)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), NULL);
  gtk_widget_set_visible (self->error_label, FALSE);
}

/* The account a widget in a row is about, which is the id of the account
   the row was built from. */
static const char *
account_id_of (gpointer widget)
{
  return g_object_get_data (G_OBJECT (widget), "aml-account-id");
}

static void
on_row_activated (AdwActionRow *row, gpointer user_data)
{
  AmlUsersPopover *self = user_data;
  const char *id = account_id_of (row);
  g_autoptr (GError) error = NULL;

  if (id == NULL)
    return;

  /* One account is active and it is one of them: the store keeps that
     true, and this only says which one. Clicking the row that is already
     active changes nothing, which the store says by not being changed. */
  if (!aml_accounts_set_active (self->accounts, id, &error))
    show_error (self, error->message);
}

static void
on_remove_clicked (GtkButton *button, gpointer user_data)
{
  AmlUsersPopover *self = user_data;
  const char *id = account_id_of (button);
  g_autoptr (GError) error = NULL;

  if (id == NULL)
    return;

  if (!aml_accounts_remove (self->accounts, id, &error))
    show_error (self, error->message);
}

/* One account as a row of the list. The whole row chooses the account it
 * is about: a click anywhere on it is the choice, rather than a small
 * radio at the left that has to be aimed at. The account that is active
 * carries a check mark, and the trash button takes an account out. */
static void
append_account_row (AmlUsersPopover *self, const AmlAccount *account)
{
  gboolean is_active =
    g_str_equal (aml_accounts_get_active_id (self->accounts) != NULL
                   ? aml_accounts_get_active_id (self->accounts)
                   : "",
                 account->id);
  gboolean needs_sign_in =
    account->type == AML_ACCOUNT_MICROSOFT &&
    !aml_account_token_is_valid (account, g_get_real_time () / 1000000);

  GtkWidget *row = adw_action_row_new ();
  GtkWidget *image = gtk_image_new ();
  GtkWidget *remove = gtk_button_new_from_icon_name ("user-trash-symbolic");
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
                               needs_sign_in ? "Microsoft · Needs sign-in"
                                             : aml_account_type_name (
                                                   account->type));

  if (needs_sign_in)
    gtk_widget_add_css_class (row, "warning");

  set_avatar_icon (GTK_IMAGE (image));
  set_avatar (self, GTK_IMAGE (image), account->avatar_url);
  gtk_image_set_pixel_size (GTK_IMAGE (image), AVATAR_SIZE);
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), image);

  /* The mark of the one a launch plays as, at the far end of the row. */
  if (is_active)
    {
      GtkWidget *check =
        gtk_image_new_from_icon_name ("object-select-symbolic");

      gtk_widget_add_css_class (check, "accent");
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), check);
    }

  tooltip = g_strdup_printf ("Remove \"%s\"", account->name);
  gtk_widget_set_tooltip_text (remove, tooltip);
  gtk_accessible_update_property (GTK_ACCESSIBLE (remove),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
  gtk_widget_add_css_class (remove, "flat");
  gtk_widget_set_valign (remove, GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (remove), "aml-account-id",
                          g_strdup (account->id), g_free);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_clicked), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), remove);

  /* Which account a row is about travels with the row: the list is built
     again whenever the store changes, and a row is never asked about
     anything else. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data_full (G_OBJECT (row), "aml-account-id",
                          g_strdup (account->id), g_free);
  g_signal_connect (row, "activated", G_CALLBACK (on_row_activated), self);

  gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
}

/* ------------------------------------------------------ *
 * Adding one
 * ------------------------------------------------------ */

/* An account the user just signed in to, or typed a name for, goes into
   the store, which is what makes it the active one and writes it down. */
static void
add_account (AmlUsersPopover *self, AmlAccount *account)
{
  g_autoptr (GError) error = NULL;

  if (!aml_accounts_add (self->accounts, account, &error))
    show_error (self, error->message);
  else
    clear_error (self);
}

static void
on_signed_in (AmlSignInDialog *dialog, AmlAccount *account,
              gpointer user_data)
{
  AmlUsersPopover *self = user_data;

  add_account (self, account);
  adw_dialog_close (ADW_DIALOG (dialog));
  gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_offline_added (AmlOfflineAccountDialog *dialog, AmlAccount *account,
                  gpointer user_data)
{
  AmlUsersPopover *self = user_data;

  add_account (self, account);
  adw_dialog_close (ADW_DIALOG (dialog));
  gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_add_microsoft_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlUsersPopover *self = user_data;
  AmlSignInDialog *dialog = aml_sign_in_dialog_new ();

  /* Weak, so that the handler does not keep the dialog alive for as long
     as it is connected to it. */
  g_signal_connect_object (dialog, "signed-in", G_CALLBACK (on_signed_in),
                           self, 0);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
  gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_add_offline_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlUsersPopover *self = user_data;
  AmlOfflineAccountDialog *dialog = aml_offline_account_dialog_new ();

  g_signal_connect_object (dialog, "account-added",
                           G_CALLBACK (on_offline_added), self, 0);

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
rebuild (AmlUsersPopover *self)
{
  GPtrArray *accounts = aml_accounts_list (self->accounts);
  GtkWidget *child;

  /* Whatever was still on its way belongs to rows that are about to be
     gone. */
  g_cancellable_cancel (self->avatars);
  g_clear_object (&self->avatars);
  self->avatars = g_cancellable_new ();

  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  gtk_widget_set_visible (self->empty_label, accounts->len == 0);

  for (guint i = 0; i < accounts->len; i++)
    append_account_row (self, g_ptr_array_index (accounts, i));

  update_the_button (self);
}

static void
on_accounts_changed (AmlAccounts *accounts G_GNUC_UNUSED,
                     gpointer    user_data)
{
  rebuild (user_data);
}

static void
aml_users_popover_init (AmlUsersPopover *self)
{
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  GtkWidget *rows = gtk_list_box_new ();
  GtkWidget *microsoft = gtk_button_new_with_label ("Add Microsoft Account");
  GtkWidget *offline = gtk_button_new_with_label ("Add Offline Account");

  /* A row is chosen by clicking it, not by a selection the list keeps:
     the active account is the store's business. The list scrolls instead
     of the popover growing without end when there are many accounts. */
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (rows), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (rows, "boxed-list");
  gtk_widget_set_size_request (scrolled, USERS_WIDTH, -1);
  gtk_scrolled_window_set_propagate_natural_width (
      GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_widget_set_halign (scrolled, GTK_ALIGN_CENTER);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), rows);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled),
                                              420);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_widget_set_size_request (rows, USERS_ROWS_WIDTH, -1);

  self->list_box = rows;

  self->empty_label = gtk_label_new (
      "No accounts yet. Add one to play as somebody of your own.");
  gtk_label_set_wrap (GTK_LABEL (self->empty_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->empty_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_start (self->empty_label, 12);
  gtk_widget_set_margin_end (self->empty_label, 12);
  gtk_widget_set_margin_top (self->empty_label, 12);
  gtk_widget_set_margin_bottom (self->empty_label, 12);

  self->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->error_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (self->error_label, "error");
  gtk_widget_set_margin_start (self->error_label, 12);
  gtk_widget_set_margin_end (self->error_label, 12);
  gtk_widget_set_margin_bottom (self->error_label, 6);
  gtk_widget_set_visible (self->error_label, FALSE);

  /* The two ways of having an account. The first one signs in without a
     password: the user types a code into a page of Microsoft's own. */
  gtk_widget_add_css_class (microsoft, "flat");
  gtk_widget_add_css_class (offline, "flat");
  gtk_widget_set_halign (microsoft, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (offline, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (microsoft, 6);
  gtk_widget_set_margin_bottom (offline, 12);
  g_signal_connect (microsoft, "clicked",
                    G_CALLBACK (on_add_microsoft_clicked), self);
  g_signal_connect (offline, "clicked", G_CALLBACK (on_add_offline_clicked),
                    self);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), scrolled);
  gtk_box_append (GTK_BOX (content), self->empty_label);
  gtk_box_append (GTK_BOX (content), microsoft);
  gtk_box_append (GTK_BOX (content), offline);
  gtk_box_append (GTK_BOX (content), self->error_label);

  gtk_popover_set_child (GTK_POPOVER (self), content);

  /* What the header says about who is playing: the avatar of the active
     account, or the default one when there is none. It is filled from the
     same store as the list, so the two cannot disagree. */
  self->avatar_image = gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->avatar_image),
                            HEADER_AVATAR_SIZE);
  gtk_widget_set_valign (self->avatar_image, GTK_ALIGN_CENTER);

  self->button_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (self->button_content), self->avatar_image);
}

static void
aml_users_popover_constructed (GObject *object)
{
  AmlUsersPopover *self = AML_USERS_POPOVER (object);

  /* Weak, so that the handler does not keep the store alive for as long
     as the popover is connected to it. */
  g_signal_connect_object (self->accounts, "changed",
                           G_CALLBACK (on_accounts_changed), self, 0);

  G_OBJECT_CLASS (aml_users_popover_parent_class)->constructed (object);

  self->avatars = g_cancellable_new ();
  rebuild (self);
}

static void
aml_users_popover_dispose (GObject *object)
{
  AmlUsersPopover *self = AML_USERS_POPOVER (object);

  /* An avatar that is still on its way is not needed by anything any
     more. */
  g_clear_object (&self->avatars);

  G_OBJECT_CLASS (aml_users_popover_parent_class)->dispose (object);
}

static void
aml_users_popover_finalize (GObject *object)
{
  AmlUsersPopover *self = AML_USERS_POPOVER (object);

  g_clear_object (&self->accounts);
  g_clear_object (&self->button_content);
  g_free (self->active_name);

  G_OBJECT_CLASS (aml_users_popover_parent_class)->finalize (object);
}

enum {
  PROP_ACCOUNTS = 1,
  PROP_LAST
};

static void
aml_users_popover_set_accounts (AmlUsersPopover *self, AmlAccounts *accounts);

static void
aml_users_popover_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  AmlUsersPopover *self = AML_USERS_POPOVER (object);

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
aml_users_popover_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  AmlUsersPopover *self = AML_USERS_POPOVER (object);

  switch (property_id)
    {
    case PROP_ACCOUNTS:
      aml_users_popover_set_accounts (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
aml_users_popover_class_init (AmlUsersPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = aml_users_popover_constructed;
  object_class->dispose = aml_users_popover_dispose;
  object_class->finalize = aml_users_popover_finalize;
  object_class->get_property = aml_users_popover_get_property;
  object_class->set_property = aml_users_popover_set_property;

  /* The store has to be there before the popover is built, because what
     is built is what the store holds. */
  g_object_class_install_property (
      object_class, PROP_ACCOUNTS,
      g_param_spec_object ("accounts", NULL, NULL, AML_TYPE_ACCOUNTS,
                           G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
}

/* The store is taken by reference and never let go of before the
   popover is: it is what the whole widget is built from. */
static void
aml_users_popover_set_accounts (AmlUsersPopover *self, AmlAccounts *accounts)
{
  g_set_object (&self->accounts, accounts);
}

AmlUsersPopover *
aml_users_popover_new (AmlAccounts *accounts)
{
  g_return_val_if_fail (AML_IS_ACCOUNTS (accounts), NULL);

  return g_object_new (AML_TYPE_USERS_POPOVER, "accounts", accounts, NULL);
}

AmlAccounts *
aml_users_popover_get_accounts (AmlUsersPopover *self)
{
  g_return_val_if_fail (AML_IS_USERS_POPOVER (self), NULL);

  return self->accounts;
}

char *
aml_users_popover_get_active_name (AmlUsersPopover *self)
{
  g_return_val_if_fail (AML_IS_USERS_POPOVER (self), NULL);

  return g_strdup (self->active_name);
}

/* The avatar and the name in the header, which is what the menu button of
 * the popover shows. It is built here rather than in the init because it
 * belongs to the popover and is handed over by the window. */
GtkWidget *
aml_users_popover_get_button_content (AmlUsersPopover *self)
{
  g_return_val_if_fail (AML_IS_USERS_POPOVER (self), NULL);

  /* The same widget every time: a caller that asked twice is holding two
     references to one button, which is what it would want, and never two
     buttons over one avatar. */
  return g_object_ref (self->button_content);
}
