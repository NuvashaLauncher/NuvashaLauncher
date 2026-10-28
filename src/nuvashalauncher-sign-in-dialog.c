/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-sign-in-dialog.h"
#include "nuvashalauncher-dialogs.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"

#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-operation.h"

/* Status messages shown while requesting and using the device code. */
#define STEP_CODE _("asking Microsoft for a code")
#define STEP_SIGN_IN _("waiting for the sign-in in the browser")

struct _nuvashalauncherSignInDialog
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
  GtkBox *body;

  /* Device-code state used while the user signs in in the browser. */
  nuvashalauncherDeviceCode *code;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherSignInDialog, nuvashalauncher_sign_in_dialog, ADW_TYPE_DIALOG)

static guint nuvashalauncher_sign_in_dialog_signals[nuvashalauncher_SIGN_IN_DIALOG_N_SIGNALS];

static void begin_sign_in (nuvashalauncherSignInDialog *self);

static void finish_sign_in (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data);

/* Show the service error and the retry action. */
static void
show_error (nuvashalauncherSignInDialog *self, const char *message)
{
  const char *friendly = message;

  if (message != NULL && (g_strstr_len (message, -1, "invalid_grant") != NULL ||
                          g_strstr_len (message, -1, "AADSTS70000") != NULL))
    friendly = _("The sign-in request expired. Start again to get a new code.");
  else if (message != NULL && g_strstr_len (message, -1, "Network") != NULL)
    friendly = _("Microsoft could not be reached. Check your connection and try again.");
  else if (message != NULL)
    friendly = _("Microsoft sign-in failed. Try again.");

  if (message != NULL && friendly != message)
    g_debug ("Microsoft sign-in failed: %s", message);
  gtk_label_set_text (GTK_LABEL (self->error_label), friendly);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), message != NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_button), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->waiting), FALSE);
}

static void
clear_error (nuvashalauncherSignInDialog *self)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_button), FALSE);
}

/* Show the current sign-in step. */
static void
set_step (nuvashalauncherSignInDialog *self, const char *step)
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

/* Open Microsoft's page through GIO, not through a command found on PATH.
   The dialog also displays the address for manual opening. */
static void
open_the_page (nuvashalauncherSignInDialog *self)
{
  g_autoptr (GError) error = NULL;

  if (self->code == NULL)
    return;

  if (!is_safe_verification_uri (self->code->verification_uri))
    {
       show_error (self, _("Microsoft returned an unsafe verification address"));
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
  g_autoptr (nuvashalauncherSignInDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  nuvashalauncherDeviceCode *code = NULL;

  code = nuvashalauncher_auth_device_code_finish (result, &error);

  if (code == NULL)
    {
      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      show_error (self, error != NULL
                          ? error->message
                           : _("Microsoft would not give a code to sign in with"));
      return;
    }

  g_clear_pointer (&self->code, nuvashalauncher_device_code_free);
  self->code = code;

  /* Show the code and verification URI. */
  gtk_label_set_text (GTK_LABEL (self->code_label), self->code->user_code);
  gtk_label_set_text (GTK_LABEL (self->uri_label),
                      self->code->verification_uri);

  gtk_widget_set_visible (GTK_WIDGET (self->instruction_label), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->code_label), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->open_button), TRUE);

  /* The page opens by itself, and the button is there for a user who
     closed the tab it opened. */
  open_the_page (self);

  set_step (self, STEP_SIGN_IN);

  nuvashalauncher_operation_release (
    nuvashalauncher_auth_sign_in_async (self->code, finish_sign_in, g_object_ref (self)),
    self->cancellable);
}

static void
finish_sign_in (GObject      *source G_GNUC_UNUSED,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (nuvashalauncherSignInDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherAccount) account = nuvashalauncher_auth_sign_in_finish (result, &error);

  if (account == NULL)
    {

      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      show_error (self, error != NULL
                          ? error->message
                           : _("the sign-in did not finish"));
      return;
    }

  g_signal_emit (self,
                 nuvashalauncher_sign_in_dialog_signals[nuvashalauncher_SIGN_IN_DIALOG_SIGNAL_SIGNED_IN],
                 0, account);
}

/* Request a device code and restart the sign-in flow. */
static void
begin_sign_in (nuvashalauncherSignInDialog *self)
{
  g_cancellable_cancel (self->cancellable);

  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_clear_pointer (&self->code, nuvashalauncher_device_code_free);

  clear_error (self);
  gtk_widget_set_visible (GTK_WIDGET (self->instruction_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->code_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->uri_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->open_button), FALSE);
  set_step (self, STEP_CODE);

  nuvashalauncher_operation_release (
    nuvashalauncher_auth_device_code_async (on_code_ready, g_object_ref (self)),
    self->cancellable);
}

void
nuvashalauncher_sign_in_dialog_retry (nuvashalauncherSignInDialog *self)
{
  g_return_if_fail (nuvashalauncher_IS_SIGN_IN_DIALOG (self));

  begin_sign_in (self);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncher_sign_in_dialog_retry (user_data);
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
  nuvashalauncherSignInDialog *self = nuvashalauncher_SIGN_IN_DIALOG (user_data);

  g_cancellable_cancel (self->cancellable);
}

/* A button with an icon and a word, as used for actions that do
 * one clear thing is written. */
static void
nuvashalauncher_sign_in_dialog_init (nuvashalauncherSignInDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
  gtk_widget_set_size_request (GTK_WIDGET (self), 360, 280);
  adw_dialog_set_follows_content_size (ADW_DIALOG (self), TRUE);
  nuvashalauncher_dialog_add_content_margin_breakpoint (ADW_DIALOG (self),
                                                GTK_WIDGET (self->body),
                                                440, 12);
  self->cancellable = g_cancellable_new ();

  gtk_label_set_text (self->instruction_label,
                      _("Open the page in your browser, then enter this code:"));
  gtk_label_set_wrap (self->instruction_label, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->instruction_label), "title-3");
  gtk_widget_set_halign (GTK_WIDGET (self->instruction_label), GTK_ALIGN_CENTER);
  gtk_label_set_selectable (self->code_label, TRUE);
  gtk_label_set_xalign (self->code_label, 0.5f);
  gtk_widget_set_halign (GTK_WIDGET (self->code_label), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (self->code_label), FALSE);
   gtk_widget_add_css_class (GTK_WIDGET (self->code_label), "title-1");
   gtk_widget_add_css_class (GTK_WIDGET (self->code_label), "monospace");
   gtk_widget_add_css_class (GTK_WIDGET (self->code_label), "card");
   gtk_widget_add_css_class (GTK_WIDGET (self->code_label), "nuvashalauncher-sign-in-code");
  gtk_widget_add_css_class (GTK_WIDGET (self->open_button), "pill");
  gtk_widget_add_css_class (GTK_WIDGET (self->open_button), "suggested-action");
  gtk_widget_set_halign (GTK_WIDGET (self->open_button), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (GTK_WIDGET (self->open_button), TRUE);
  gtk_widget_set_margin_start (GTK_WIDGET (self->open_button), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->open_button), 12);
  gtk_widget_add_css_class (GTK_WIDGET (self->uri_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->uri_label), "caption");
  gtk_label_set_selectable (self->uri_label, TRUE);
  gtk_label_set_justify (self->uri_label, GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (self->uri_label, 0.5f);
  gtk_label_set_wrap (self->uri_label, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->waiting_label), "dim-label");
  gtk_widget_set_halign (GTK_WIDGET (self->waiting), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self->waiting), GTK_ALIGN_CENTER);
  gtk_label_set_wrap (self->error_label, TRUE);
  gtk_label_set_xalign (self->error_label, 0.5f);
  gtk_label_set_justify (self->error_label, GTK_JUSTIFY_CENTER);
   gtk_widget_set_halign (GTK_WIDGET (self->retry_button), GTK_ALIGN_CENTER);

  gtk_widget_set_visible (GTK_WIDGET (self->instruction_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->code_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->open_button), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->uri_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_button), FALSE);

   gtk_button_set_label (self->open_button, _("Open in browser"));
  gtk_button_set_label (self->retry_button, _("Try Again"));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (self->retry_button), _("Try Again"));
  g_signal_connect_object (self->open_button, "clicked", G_CALLBACK (on_open_clicked), self, 0);
  g_signal_connect_object (self->retry_button, "clicked", G_CALLBACK (on_retry_clicked), self, 0);
}

static void
nuvashalauncher_sign_in_dialog_dispose (GObject *object)
{
  nuvashalauncherSignInDialog *self = nuvashalauncher_SIGN_IN_DIALOG (object);

  /* A dialog that is going away stops waiting: the code stops being
     polled and nothing comes back to widgets that are not on screen. */
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (nuvashalauncher_sign_in_dialog_parent_class)->dispose (object);
}

static void
nuvashalauncher_sign_in_dialog_finalize (GObject *object)
{
  nuvashalauncherSignInDialog *self = nuvashalauncher_SIGN_IN_DIALOG (object);

  g_clear_pointer (&self->code, nuvashalauncher_device_code_free);

  G_OBJECT_CLASS (nuvashalauncher_sign_in_dialog_parent_class)->finalize (object);
}

static void
nuvashalauncher_sign_in_dialog_class_init (nuvashalauncherSignInDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class,
      "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-sign-in.ui");
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, spinner);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, waiting);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, waiting_label);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, instruction_label);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, code_label);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, uri_label);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, open_button);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, error_label);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, retry_button);
  gtk_widget_class_bind_template_child (widget_class, nuvashalauncherSignInDialog, body);

  object_class->dispose = nuvashalauncher_sign_in_dialog_dispose;
  object_class->finalize = nuvashalauncher_sign_in_dialog_finalize;

  nuvashalauncher_sign_in_dialog_signals[nuvashalauncher_SIGN_IN_DIALOG_SIGNAL_SIGNED_IN] =
    g_signal_new ("signed-in", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 1, nuvashalauncher_TYPE_ACCOUNT);
}

nuvashalauncherSignInDialog *
nuvashalauncher_sign_in_dialog_new (void)
{
  nuvashalauncherSignInDialog *self = g_object_new (nuvashalauncher_TYPE_SIGN_IN_DIALOG, NULL);

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
