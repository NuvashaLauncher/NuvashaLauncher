/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-sign-in-dialog.h"

#include "aml-auth-microsoft.h"

/* What the dialog is waiting for while there is nothing to show: the
 * code from Microsoft, and then the user with it. */
#define STEP_CODE "asking Microsoft for a code"
#define STEP_SIGN_IN "waiting for the sign-in in the browser"

struct _AmlSignInDialog
{
  AdwDialog parent_instance;

  GCancellable *cancellable;

  GtkWidget *spinner;
  GtkWidget *step_label;
  GtkWidget *code_label;
  GtkWidget *uri_label;
  GtkWidget *open_button;
  GtkWidget *error_label;
  GtkWidget *retry_button;

  /* What the chain polls with while the user is in the browser. It is
     freed with the dialog and never shown: the user types the code that
     belongs to it, not this. */
  AmlDeviceCode *code;
};

G_DEFINE_FINAL_TYPE (AmlSignInDialog, aml_sign_in_dialog, ADW_TYPE_DIALOG)

static guint aml_sign_in_dialog_signals[AML_SIGN_IN_DIALOG_N_SIGNALS];

static void begin_sign_in (AmlSignInDialog *self);

static void finish_sign_in (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data);

/* What went wrong, said in the words of the service that refused, with a
   way to ask for a new code next to it. */
static void
show_error (AmlSignInDialog *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), message);
  gtk_widget_set_visible (self->error_label, TRUE);
  gtk_widget_set_visible (self->retry_button, TRUE);
  gtk_widget_set_visible (self->spinner, FALSE);
}

static void
clear_error (AmlSignInDialog *self)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), NULL);
  gtk_widget_set_visible (self->error_label, FALSE);
  gtk_widget_set_visible (self->retry_button, FALSE);
}

/* What is being waited for. There is nothing of a token or of an account
   in this line: it names the step, never what the step answered. */
static void
set_step (AmlSignInDialog *self, const char *step)
{
  gtk_label_set_text (GTK_LABEL (self->step_label), step);
  gtk_widget_set_visible (self->spinner, TRUE);
}

/* Opens the page Microsoft serves through GIO, the way the desktop opens
   any other link: never through a command that would run whatever is
   under that name on the PATH. The address is on the dialog as well, for
   a user who would rather open it themselves. */
static void
open_the_page (AmlSignInDialog *self)
{
  g_autoptr (GError) error = NULL;

  if (self->code == NULL)
    return;

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
  g_autoptr (AmlSignInDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  AmlDeviceCode *code = NULL;

  code = aml_auth_device_code_finish (result, &error);

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

  g_clear_pointer (&self->code, aml_device_code_free);
  self->code = code;

  /* The code the user types, and where they type it. What Microsoft says
     about it is the sentence to show: it is written for this moment. */
  gtk_label_set_text (GTK_LABEL (self->code_label), self->code->user_code);
  gtk_label_set_text (GTK_LABEL (self->uri_label),
                      self->code->verification_uri);

  if (self->code->message != NULL && *self->code->message != '\0')
    gtk_label_set_text (GTK_LABEL (self->step_label), self->code->message);
  else
    set_step (self, STEP_SIGN_IN);

  gtk_widget_set_visible (self->code_label, TRUE);
  gtk_widget_set_visible (self->uri_label, TRUE);

  /* The page opens by itself, and the button is there for a user who
     closed the tab it opened. */
  open_the_page (self);
  gtk_widget_set_visible (self->open_button, TRUE);

  aml_auth_sign_in_async (self->code, self->cancellable, finish_sign_in,
                          g_object_ref (self));
}

static void
finish_sign_in (GObject      *source G_GNUC_UNUSED,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (AmlSignInDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlAccount) account = aml_auth_sign_in_finish (result, &error);

  if (account == NULL)
    {
      /* Closing the dialog is the user saying no, which is not a failure
         worth a sentence. Anything else is what the service said, in the
         words it said it in. */
      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      show_error (self, error != NULL
                          ? error->message
                          : "the sign-in did not finish");
      return;
    }

  g_signal_emit (self,
                 aml_sign_in_dialog_signals[AML_SIGN_IN_DIALOG_SIGNAL_SIGNED_IN],
                 0, account);
}

/* Asks Microsoft for a code and starts the wait over: what the Try again
   button does, and what the dialog does when it is presented. */
static void
begin_sign_in (AmlSignInDialog *self)
{
  g_cancellable_cancel (self->cancellable);

  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_clear_pointer (&self->code, aml_device_code_free);

  clear_error (self);
  gtk_widget_set_visible (self->code_label, FALSE);
  gtk_widget_set_visible (self->uri_label, FALSE);
  gtk_widget_set_visible (self->open_button, FALSE);
  set_step (self, STEP_CODE);

  aml_auth_device_code_async (self->cancellable, on_code_ready,
                              g_object_ref (self));
}

void
aml_sign_in_dialog_retry (AmlSignInDialog *self)
{
  g_return_if_fail (AML_IS_SIGN_IN_DIALOG (self));

  begin_sign_in (self);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  aml_sign_in_dialog_retry (user_data);
}

static void
on_open_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  open_the_page (user_data);
}

static void
on_close_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  adw_dialog_close (ADW_DIALOG (user_data));
}

/* Closing the dialog stops the waiting: the token endpoint is not polled
   for a sign-in nobody is looking at any more. */
static void
on_closed (AdwDialog *dialog G_GNUC_UNUSED, gpointer user_data)
{
  AmlSignInDialog *self = user_data;

  g_cancellable_cancel (self->cancellable);
}

static void
aml_sign_in_dialog_init (AmlSignInDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), "Sign In");
  adw_dialog_set_content_width (ADW_DIALOG (self), 420);

  self->cancellable = g_cancellable_new ();

  /* What is being waited for, and a spinner while it is. */
  self->spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_widget_set_valign (self->spinner, GTK_ALIGN_CENTER);

  self->step_label = gtk_label_new (STEP_CODE);
  gtk_label_set_wrap (GTK_LABEL (self->step_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->step_label), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (self->step_label), 0.5);

  /* The code itself, big enough to be read off the screen and typed into
     a page of Microsoft's own. */
  self->code_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->code_label, "title-1");
  /* The code is there to be read and typed somewhere else: letting it be
     selected means it can be copied instead. */
  gtk_label_set_selectable (GTK_LABEL (self->code_label), TRUE);
  gtk_widget_set_visible (self->code_label, FALSE);

  self->uri_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->uri_label, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (self->uri_label), TRUE);
  gtk_label_set_selectable (GTK_LABEL (self->uri_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->uri_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_visible (self->uri_label, FALSE);

  self->open_button = gtk_button_new_with_label ("Open the page again");
  gtk_widget_add_css_class (self->open_button, "flat");
  gtk_widget_set_halign (self->open_button, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->open_button, FALSE);
  g_signal_connect (self->open_button, "clicked", G_CALLBACK (on_open_clicked),
                    self);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_valign (header, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (header), self->spinner);
  gtk_box_append (GTK_BOX (header), self->code_label);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (body, 24);
  gtk_widget_set_margin_end (body, 24);
  gtk_widget_set_margin_top (body, 24);
  gtk_widget_set_margin_bottom (body, 12);
  gtk_box_append (GTK_BOX (body), header);
  gtk_box_append (GTK_BOX (body), self->step_label);
  gtk_box_append (GTK_BOX (body), self->uri_label);
  gtk_box_append (GTK_BOX (body), self->open_button);

  self->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->error_label), 0.5);
  gtk_label_set_justify (GTK_LABEL (self->error_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (self->error_label, "error");
  gtk_widget_set_margin_start (self->error_label, 24);
  gtk_widget_set_margin_end (self->error_label, 24);
  gtk_box_append (GTK_BOX (body), self->error_label);

  self->retry_button = gtk_button_new_with_label ("Try again");
  gtk_widget_add_css_class (self->retry_button, "flat");
  gtk_widget_set_halign (self->retry_button, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->retry_button, FALSE);
  g_signal_connect (self->retry_button, "clicked", G_CALLBACK (on_retry_clicked),
                    self);
  gtk_box_append (GTK_BOX (body), self->retry_button);

  GtkWidget *close_button = gtk_button_new_with_label ("Cancel");
  g_signal_connect (close_button, "clicked", G_CALLBACK (on_close_clicked),
                    self);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_start (buttons, 24);
  gtk_widget_set_margin_end (buttons, 24);
  gtk_widget_set_margin_bottom (buttons, 24);
  gtk_box_append (GTK_BOX (buttons), close_button);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), body);
  gtk_box_append (GTK_BOX (content), buttons);

  adw_dialog_set_child (ADW_DIALOG (self), content);
}

static void
aml_sign_in_dialog_dispose (GObject *object)
{
  AmlSignInDialog *self = AML_SIGN_IN_DIALOG (object);

  /* A dialog that is going away stops waiting: the code stops being
     polled and nothing comes back to widgets that are not on screen. */
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (aml_sign_in_dialog_parent_class)->dispose (object);
}

static void
aml_sign_in_dialog_finalize (GObject *object)
{
  AmlSignInDialog *self = AML_SIGN_IN_DIALOG (object);

  g_clear_pointer (&self->code, aml_device_code_free);

  G_OBJECT_CLASS (aml_sign_in_dialog_parent_class)->finalize (object);
}

static void
aml_sign_in_dialog_class_init (AmlSignInDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_sign_in_dialog_dispose;
  object_class->finalize = aml_sign_in_dialog_finalize;

  aml_sign_in_dialog_signals[AML_SIGN_IN_DIALOG_SIGNAL_SIGNED_IN] =
    g_signal_new ("signed-in", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 1, AML_TYPE_ACCOUNT);
}

AmlSignInDialog *
aml_sign_in_dialog_new (void)
{
  AmlSignInDialog *self = g_object_new (AML_TYPE_SIGN_IN_DIALOG, NULL);

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
