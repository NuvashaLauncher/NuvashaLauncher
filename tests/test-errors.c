/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "nuvashalauncher-errors.h"
#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-offline-account-dialog.h"

static void
test_auth_errors_are_typed (void)
{
  g_autoptr (GError) error = NULL;
  gboolean slow = FALSE;
  nuvashalauncher_auth_polling_error (
    "{\"error\":\"invalid_grant\",\"error_description\":\"Network expired\"}", -1, &slow, &error);
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_REAUTH);
  g_assert_cmpstr (nuvashalauncher_error_message (error), ==,
                   "Your saved sign-in is no longer valid. Sign in again.");
  g_clear_error (&error);
  error = g_error_new_literal (G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "untranslated DNS diagnostic");
  g_assert_cmpstr (nuvashalauncher_error_message (error), ==,
                   "The connection failed. Check your internet connection and try again.");
  g_assert_cmpstr (error->message, ==, "untranslated DNS diagnostic");
}

static void
test_error_categories (void)
{
  g_autoptr (GError) disk = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NO_SPACE, "raw disk failure");
  g_autoptr (GError) hash = g_error_new_literal (nuvashalauncher_DOWNLOAD_ERROR,
    nuvashalauncher_DOWNLOAD_ERROR_CHECKSUM, "raw checksum mismatch");
  g_autoptr (GError) network = g_error_new_literal (nuvashalauncher_MODRINTH_ERROR,
    nuvashalauncher_MODRINTH_ERROR_NETWORK, "raw connection failure");
  g_autoptr (GError) cancel = g_error_new_literal (nuvashalauncher_DOWNLOAD_ERROR,
    nuvashalauncher_DOWNLOAD_ERROR_CANCELLED, "cancelled");
  g_assert_cmpstr (nuvashalauncher_error_message (disk), ==, "There is not enough free disk space.");
  g_assert_nonnull (strstr (nuvashalauncher_error_message (hash), "failed verification"));
  g_assert_null (strstr (nuvashalauncher_error_message (network), "compatible"));
  g_assert_true (nuvashalauncher_error_is_cancelled (cancel));
}

static GtkWidget *
find_type (GtkWidget *widget, GType type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (widget, type))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *match = find_type (child, type);
      if (match != NULL)
        return match;
    }
  return NULL;
}

static gboolean
has_visible_label (GtkWidget *widget, const char *text)
{
  if (GTK_IS_LABEL (widget) && gtk_widget_get_visible (widget) &&
      g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), text) == 0)
    return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    if (has_visible_label (child, text))
      return TRUE;
  return FALSE;
}

static void
test_error_widgets (void)
{
  if (!gtk_init_check ())
    { g_test_skip ("No display"); return; }
  g_autoptr (GtkWidget) banner = g_object_ref_sink (adw_banner_new (""));
  g_autoptr (GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                                "<b>literal diagnostic</b>");
  nuvashalauncher_ui_error_banner (ADW_BANNER (banner), error);
  g_assert_true (adw_banner_get_revealed (ADW_BANNER (banner)));
  g_assert_false (adw_banner_get_use_markup (ADW_BANNER (banner)));
  g_autoptr (GtkWidget) details = g_object_ref_sink (nuvashalauncher_ui_error_details (error));
  GtkWidget *label = gtk_expander_get_child (GTK_EXPANDER (details));
  g_assert_false (gtk_label_get_use_markup (GTK_LABEL (label)));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==,
                   "Access was denied. Check the folder permissions.");

  g_autoptr (nuvashalauncherOfflineAccountDialog) dialog = g_object_ref_sink (
    nuvashalauncher_offline_account_dialog_new ());
  GtkWidget *content = adw_dialog_get_child (ADW_DIALOG (dialog));
  GtkWidget *entry = find_type (content, ADW_TYPE_ENTRY_ROW);
  g_assert_nonnull (entry);
  gtk_editable_set_text (GTK_EDITABLE (entry), "a!");
  g_assert_true (has_visible_label (content, "a name has to be 3 to 16 characters long"));
  gtk_editable_set_text (GTK_EDITABLE (entry), "Valid_Name");
  g_assert_false (has_visible_label (content, "a name has to be 3 to 16 characters long"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/errors/auth-types", test_auth_errors_are_typed);
  g_test_add_func ("/errors/categories", test_error_categories);
  g_test_add_func ("/errors/widgets", test_error_widgets);
  return g_test_run ();
}
