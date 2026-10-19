/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-sign-in-dialog.h"
#include "nuvasha-i18n.h"

#include "nuvasha-auth-microsoft.h"
#include "nuvasha-operation.h"

/* Status messages shown while requesting and using the device code. */
#define STEP_CODE _("asking Microsoft for a code")
#define STEP_SIGN_IN _("waiting for the sign-in in the browser")

struct _NuvashaSignInDialog
{
  AdwDialog parent_instance;

  GCancellable *cancellable;

  AdwSpinner *spinner;
  GtkBox *waiting;              /* spinner and status message */
  GtkLabel *waiting_label;
  GtkLabel *instruction_label;  /* instructions for the code */
  GtkLabel *code_label;
  GtkLabel *uri_label;
  GtkButton *open_button;
  GtkLabel *error_label;
  GtkButton *retry_button;

  /* Device-code state used while the user signs in in the browser. */
  NuvashaDeviceCode *code;
};

G_DEFINE_FINAL_TYPE (NuvashaSignInDialog, nuvasha_sign_in_dialog, ADW_TYPE_DIALOG)

static guint nuvasha_sign_in_dialog_signals[nuvasha_SIGN_IN_DIALOG_N_SIGNALS];

static void begin_sign_in (NuvashaSignInDialog *self);

static void finish_sign_in (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data);

/* Show the service error and the retry action. */
static void
show_error (NuvashaSignInDialog *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), message);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), message != NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_button), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->waiting), FALSE);
}

static void
clear_error (NuvashaSignInDialog *self)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_button), FALSE);
}

/* Show the current sign-in step. */
static void
set_step (NuvashaSignInDialog *self, const char *step)
{
  gtk_label_set_text (GTK_LABEL (self->waiting_label), step);
  gtk_widget_set_visible (GTK_WIDGET (self->waiting), TRUE);
}

static gboolean
is_safe_verification_uri (const char *address)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GUri) uri = NULL;
  const char *scheme = NULL;
  const char *host = NULL;
  gint port;

  if (address == NULL)
    return FALSE;

  uri = g_uri_parse (address, G_URI_FLAGS_NONE, &error);
  if (uri == NULL)
    return FALSE;

  scheme = g_uri_get_scheme (uri);
  host = g_uri_get_host (uri);
  port = g_uri_get_port (uri);

  /* Microsoft currently returns www.microsoft.com/link, while older
   * responses used microsoft.com/devicelogin. Keep the allowlist exact: a
   * lookalike subdomain must not be enough to launch an arbitrary URL. */
  return scheme != NULL && g_ascii_strcasecmp (scheme, "https") == 0 &&
         host != NULL &&
         (g_ascii_strcasecmp (host, "microsoft.com") == 0 ||
          g_ascii_strcasecmp (host, "www.microsoft.com") == 0) &&
         (port == -1 || port == 443);
}

/* Opens the page Microsoft serves through GIO, the way the desktop opens
   any other link: never through a command that would run whatever is
   under that name on the PATH. The address is on the dialog as well, for
   a user who would rather open it themselves. */
static void
open_the_page (NuvashaSignInDialog *self)
{
  g_autoptr (GError) error = NULL;

  if (self->code == NULL)
    return;

  if (!is_safe_verification_uri (self->code->verification_uri))
    {
      show_error (self, "Microsoft returned an unsafe verification address");
      return;
    }

  if (!g_app_info_launch_default_for_uri (self->code->verification_uri, NULL,
                                          &error))
    {
      show_error (self, error->message);
      return;
    }
}

static void
on_code_ready (GObject      *source G_GNUC_UNUSED,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (NuvashaSignInDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  NuvashaDeviceCode *code = NULL;

  code = nuvasha_auth_device_code_finish (result, &error);

  if (code == NULL)
    {
      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      show_error (self, error != NULL
                          ? error->message
                          : "Microsoft would not give a code to sign in with");
      return;
    }

  g_clear_pointer (&self->code, nuvasha_device_code_free);
  self->code = code;

  /* Show the code and verification URI. */
  gtk_label_set_text (GTK_LABEL (self->code_label), self->code->user_code);
  gtk_label_set_text (GTK_LABEL (self->uri_label),
                      self->code->verification_uri);

  gtk_widget_set_visible (GTK_WIDGET (self->instruction_label), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->code_label), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->uri_label), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->open_button), TRUE);

  /* The page opens by itself, and the button is there for a user who
     closed the tab it opened. */
  open_the_page (self);

  set_step (self, STEP_SIGN_IN);

  nuvasha_operation_release (
    nuvasha_auth_sign_in_async (self->code, finish_sign_in, g_object_ref (self)),
    self->cancellable);
}

static void
finish_sign_in (GObject      *source G_GNUC_UNUSED,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (NuvashaSignInDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaAccount) account = nuvasha_auth_sign_in_finish (result, &error);

  if (account == NULL)
    {
      /* Cancellation is silent; other failures are shown to the user. */
      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      show_error (self, error != NULL
                          ? error->message
                          : "the sign-in did not finish");
      return;
    }

  g_signal_emit (self,
                 nuvasha_sign_in_dialog_signals[nuvasha_SIGN_IN_DIALOG_SIGNAL_SIGNED_IN],
                 0, account);
}

/* Request a device code and restart the sign-in flow. */
static void
begin_sign_in (NuvashaSignInDialog *self)
{
  g_cancellable_cancel (self->cancellable);

  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_clear_pointer (&self->code, nuvasha_device_code_free);

  clear_error (self);
  gtk_widget_set_visible (GTK_WIDGET (self->instruction_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->code_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->uri_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->open_button), FALSE);
  set_step (self, STEP_CODE);

  nuvasha_operation_release (
    nuvasha_auth_device_code_async (on_code_ready, g_object_ref (self)),
    self->cancellable);
}

void
nuvasha_sign_in_dialog_retry (NuvashaSignInDialog *self)
{
  g_return_if_fail (nuvasha_IS_SIGN_IN_DIALOG (self));

  begin_sign_in (self);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvasha_sign_in_dialog_retry (user_data);
}

static void
on_open_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  open_the_page (user_data);
}

/* Closing the dialog stops the waiting: the token endpoint is not polled
   for a sign-in nobody is looking at any more. */
static void
on_closed (AdwDialog *dialog G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaSignInDialog *self = nuvasha_SIGN_IN_DIALOG (user_data);

  g_cancellable_cancel (self->cancellable);
}

/* A button whose face is an icon and a word, the way a button that does
 * one clear thing is written. */
static GtkWidget *
icon_label_button (const char *icon_name, const char *label)
{
  GtkButton *button = GTK_BUTTON (gtk_button_new ());
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
  GtkImage *icon = GTK_IMAGE (gtk_image_new_from_icon_name (icon_name));
  GtkLabel *text = GTK_LABEL (gtk_label_new (label));

  gtk_widget_set_valign (GTK_WIDGET (icon), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (text), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (icon));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (text));
  gtk_button_set_child (GTK_BUTTON (GTK_WIDGET (button)), GTK_WIDGET (box));

  return GTK_WIDGET (button);
}

static void
nuvasha_sign_in_dialog_init (NuvashaSignInDialog *self)
{
  GtkWidget *body = NULL;
  GtkWidget *toolbar = NULL;
  GtkWidget *header = NULL;

  adw_dialog_set_title (ADW_DIALOG (self), _("Sign In"));
  adw_dialog_set_content_width (ADW_DIALOG (self), 440);

  self->cancellable = g_cancellable_new ();

  /* Instructions shown above the code. */
  self->instruction_label = GTK_LABEL (gtk_label_new (
      _("Open the page in your browser, then enter this code:")));
  gtk_label_set_wrap (GTK_LABEL (self->instruction_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->instruction_label),
                         GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (self->instruction_label), 0.5);
  gtk_widget_set_visible (GTK_WIDGET (self->instruction_label), FALSE);

  /* Display the code prominently in selectable monospace text. */
  self->code_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->code_label), "title-1");
  gtk_widget_add_css_class (GTK_WIDGET (self->code_label), "monospace");
  gtk_label_set_selectable (GTK_LABEL (self->code_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->code_label), 0.5);
  gtk_widget_set_visible (GTK_WIDGET (self->code_label), FALSE);

  self->open_button = GTK_BUTTON (icon_label_button ("web-browser-symbolic",
                                          _("Open the Page")));
  gtk_widget_add_css_class (GTK_WIDGET (self->open_button), "pill");
  gtk_widget_add_css_class (GTK_WIDGET (self->open_button), "suggested-action");
  gtk_widget_set_halign (GTK_WIDGET (self->open_button), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->open_button), FALSE);
  g_signal_connect (self->open_button, "clicked", G_CALLBACK (on_open_clicked),
                    self);

  /* The address the code is entered at, for a user who would rather open
     the page themselves than follow the button. */
  self->uri_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->uri_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->uri_label), "caption");
  gtk_label_set_wrap (GTK_LABEL (self->uri_label), TRUE);
  gtk_label_set_selectable (GTK_LABEL (self->uri_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->uri_label), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (self->uri_label), 0.5);
  gtk_widget_set_visible (GTK_WIDGET (self->uri_label), FALSE);

  /* Status label and spinner shown while waiting. */
  self->spinner = ADW_SPINNER (adw_spinner_new ());
  self->waiting_label = GTK_LABEL (gtk_label_new (STEP_CODE));
  gtk_widget_add_css_class (GTK_WIDGET (self->waiting_label), "dim-label");
  self->waiting = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_widget_set_halign (GTK_WIDGET (self->waiting), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self->waiting), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self->waiting), GTK_WIDGET (self->spinner));
  gtk_box_append (GTK_BOX (self->waiting), GTK_WIDGET (self->waiting_label));

  /* Error message and retry action. */
  self->error_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->error_label), 0.5);
  gtk_label_set_justify (GTK_LABEL (self->error_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->error_label), "error");
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);

  self->retry_button = GTK_BUTTON (gtk_button_new_with_label (_("Try Again")));
  gtk_widget_add_css_class (GTK_WIDGET (self->retry_button), "flat");
  gtk_widget_set_halign (GTK_WIDGET (self->retry_button), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_button), FALSE);
  g_signal_connect (self->retry_button, "clicked",
                    G_CALLBACK (on_retry_clicked), self);

  body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (body, 24);
  gtk_widget_set_margin_end (body, 24);
  gtk_widget_set_margin_top (body, 24);
  gtk_widget_set_margin_bottom (body, 24);
  gtk_box_append (GTK_BOX (body), GTK_WIDGET (self->instruction_label));
  gtk_box_append (GTK_BOX (body), GTK_WIDGET (self->code_label));
  gtk_box_append (GTK_BOX (body), GTK_WIDGET (self->open_button));
  gtk_box_append (GTK_BOX (body), GTK_WIDGET (self->uri_label));
  gtk_box_append (GTK_BOX (body), GTK_WIDGET (self->waiting));
  gtk_box_append (GTK_BOX (body), GTK_WIDGET (self->error_label));
  gtk_box_append (GTK_BOX (body), GTK_WIDGET (self->retry_button));

  /* Use a header bar with only the title and close button; this dialog has
     no separate Cancel action. */
  header = adw_header_bar_new ();
  adw_header_bar_set_show_back_button (ADW_HEADER_BAR (header), FALSE);
  toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), body);

  adw_dialog_set_child (ADW_DIALOG (self), toolbar);
}

static void
nuvasha_sign_in_dialog_dispose (GObject *object)
{
  NuvashaSignInDialog *self = nuvasha_SIGN_IN_DIALOG (object);

  /* A dialog that is going away stops waiting: the code stops being
     polled and nothing comes back to widgets that are not on screen. */
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (nuvasha_sign_in_dialog_parent_class)->dispose (object);
}

static void
nuvasha_sign_in_dialog_finalize (GObject *object)
{
  NuvashaSignInDialog *self = nuvasha_SIGN_IN_DIALOG (object);

  g_clear_pointer (&self->code, nuvasha_device_code_free);

  G_OBJECT_CLASS (nuvasha_sign_in_dialog_parent_class)->finalize (object);
}

static void
nuvasha_sign_in_dialog_class_init (NuvashaSignInDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_sign_in_dialog_dispose;
  object_class->finalize = nuvasha_sign_in_dialog_finalize;

  nuvasha_sign_in_dialog_signals[nuvasha_SIGN_IN_DIALOG_SIGNAL_SIGNED_IN] =
    g_signal_new ("signed-in", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 1, nuvasha_TYPE_ACCOUNT);
}

NuvashaSignInDialog *
nuvasha_sign_in_dialog_new (void)
{
  NuvashaSignInDialog *self = g_object_new (nuvasha_TYPE_SIGN_IN_DIALOG, NULL);

  /* Weak, so that the handler does not keep the dialog alive for as long
     as it is connected to it. */
  g_signal_connect_object (self, "closed", G_CALLBACK (on_closed), self, 0);

  /* The chain starts when the dialog is made rather than when it is
     presented, so that the code is there by the time a user looks at it.
     A dialog that is never shown has asked Microsoft for one code and
     stopped there. */
  begin_sign_in (self);

  return self;
}
