/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "nuvashalauncher-errors.h"
#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-instance.h"
#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-offline-account-dialog.h"
#include <json-glib/json-glib.h>

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

  g_autoptr (NuvashaLauncherOfflineAccountDialog) dialog = g_object_ref_sink (
    nuvashalauncher_offline_account_dialog_new ());
  GtkWidget *content = adw_dialog_get_child (ADW_DIALOG (dialog));
  GtkWidget *entry = find_type (content, ADW_TYPE_ENTRY_ROW);
  g_assert_nonnull (entry);
  gtk_editable_set_text (GTK_EDITABLE (entry), "a!");
  g_assert_true (has_visible_label (content, "a name has to be 3 to 16 characters long"));
  gtk_editable_set_text (GTK_EDITABLE (entry), "Valid_Name");
  g_assert_false (has_visible_label (content, "a name has to be 3 to 16 characters long"));
}

static void
test_error_domains_exhaustive (void)
{
  /* NULL error */
  g_assert_cmpstr (nuvashalauncher_error_message (NULL), ==,
                   "The operation failed. See Technical Details for more information.");

  /* TLS Error */
  g_autoptr (GError) tls_err = g_error_new_literal (G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE, "cert failed");
  g_assert_cmpstr (nuvashalauncher_error_message (tls_err), ==,
                   "The connection failed. Check your internet connection and try again.");

  /* JSON Parser Error */
  g_autoptr (GError) json_err = g_error_new_literal (JSON_PARSER_ERROR, JSON_PARSER_ERROR_PARSE, "parse error");
  g_assert_cmpstr (nuvashalauncher_error_message (json_err), ==,
                   "The service returned an invalid response. Try again later.");

  /* G_SPAWN_ERROR */
  g_autoptr (GError) spawn_err = g_error_new_literal (G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT, "spawn failed");
  g_assert_cmpstr (nuvashalauncher_error_message (spawn_err), ==,
                   "The Java executable could not be used. Select a working Java runtime.");

  /* G_IO_ERROR codes */
  g_autoptr (GError) io_perm = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "denied");
  g_assert_cmpstr (nuvashalauncher_error_message (io_perm), ==, "Access was denied. Check the folder permissions.");

  g_autoptr (GError) io_nf = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
  g_assert_cmpstr (nuvashalauncher_error_message (io_nf), ==, "The file or folder was not found. Check its location.");

  g_autoptr (GError) io_exists = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_EXISTS, "exists");
  g_assert_cmpstr (nuvashalauncher_error_message (io_exists), ==, "The destination already exists. Choose another location.");

  g_autoptr (GError) io_fail = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "failed");
  g_assert_cmpstr (nuvashalauncher_error_message (io_fail), ==, "The operation failed. See Technical Details for more information.");

  g_autoptr (GError) io_closed = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "closed");
  g_assert_cmpstr (nuvashalauncher_error_message (io_closed), ==, "The connection failed. Check your internet connection and try again.");

  /* G_FILE_ERROR codes */
  g_autoptr (GError) file_acc = g_error_new_literal (G_FILE_ERROR, G_FILE_ERROR_ACCES, "no access");
  g_assert_cmpstr (nuvashalauncher_error_message (file_acc), ==, "Access was denied. Check the folder permissions.");

  g_autoptr (GError) file_spc = g_error_new_literal (G_FILE_ERROR, G_FILE_ERROR_NOSPC, "no space");
  g_assert_cmpstr (nuvashalauncher_error_message (file_spc), ==, "There is not enough free disk space.");

  g_autoptr (GError) file_other = g_error_new_literal (G_FILE_ERROR, G_FILE_ERROR_IO, "generic io");
  g_assert_cmpstr (nuvashalauncher_error_message (file_other), ==, "Could not read or write files. Check the location, permissions, and free disk space.");

  /* nuvashalauncher_AUTH_ERROR codes */
  g_autoptr (GError) auth_exp = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_EXPIRED, "exp");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_exp), ==, "The sign-in request expired. Start again to get a new code.");

  g_autoptr (GError) auth_dec = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_DECLINED, "dec");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_dec), ==, "Sign-in was declined. Start again if you want to sign in.");

  g_autoptr (GError) auth_nogame = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_GAME, "nogame");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_nogame), ==, "This account does not own Minecraft: Java Edition.");

  g_autoptr (GError) auth_noprof = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_PROFILE, "noprof");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_noprof), ==, "Set up a Minecraft: Java Edition profile on minecraft.net, then try again.");

  g_autoptr (GError) auth_nocli = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_CLIENT_ID, "nocli");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_nocli), ==, "Microsoft sign-in is not configured in this build. Update the launcher.");

  g_autoptr (GError) auth_xprof = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX_PROFILE, "xprof");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_xprof), ==, "this Microsoft account has no Xbox profile behind it yet: sign in to xbox.com once with it, then sign in here again");

  g_autoptr (GError) auth_xfam = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX_FAMILY, "xfam");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_xfam), ==, "this is a child account: it has to be added to a Microsoft family by an adult before it can play");

  g_autoptr (GError) auth_xctry = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX_COUNTRY, "xctry");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_xctry), ==, "Xbox Live is not available for the country this account is registered in");

  g_autoptr (GError) auth_xage = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX_AGE, "xage");
  g_assert_cmpstr (nuvashalauncher_error_message (auth_xage), ==, "this account needs an adult to verify it before it can play, which is what South Korean accounts have to do");

  /* nuvashalauncher_DOWNLOAD_ERROR codes */
  g_autoptr (GError) dl_http = g_error_new_literal (nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_HTTP, "http");
  g_assert_cmpstr (nuvashalauncher_error_message (dl_http), ==, "The service rejected the request. Try again later.");

  g_autoptr (GError) dl_rate = g_error_new_literal (nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_RATE_LIMITED, "rate");
  g_assert_cmpstr (nuvashalauncher_error_message (dl_rate), ==, "Too many requests. Wait a moment and try again.");

  /* nuvashalauncher_MODRINTH_ERROR codes */
  g_autoptr (GError) mr_nf = g_error_new_literal (nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_NOT_FOUND, "nf");
  g_assert_cmpstr (nuvashalauncher_error_message (mr_nf), ==, "The requested file, project, or compatible version was not found.");

  /* nuvashalauncher_MINECRAFT_SERVICES_ERROR */
  g_autoptr (GError) ms_rate = g_error_new_literal (nuvashalauncher_MINECRAFT_SERVICES_ERROR, nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED, "rate");
  g_assert_cmpstr (nuvashalauncher_error_message (ms_rate), ==, "Too many requests. Wait a moment and try again.");

  /* nuvashalauncher_MODPACK_ERROR */
  g_autoptr (GError) mp_dl = g_error_new_literal (nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_DOWNLOAD, "dl");
  g_assert_cmpstr (nuvashalauncher_error_message (mp_dl), ==, "Pack could not be fetched");

  g_autoptr (GError) mp_load = g_error_new_literal (nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_LOADER, "ldr");
  g_assert_cmpstr (nuvashalauncher_error_message (mp_load), ==, "Could not install this mod loader. Choose another version or inspect Technical Details.");

  /* Other subsystems */
  g_autoptr (GError) loaders_err = g_error_new_literal (nuvashalauncher_LOADERS_ERROR, 1, "ldr");
  g_assert_cmpstr (nuvashalauncher_error_message (loaders_err), ==, "Could not install this mod loader. Choose another version or inspect Technical Details.");

  g_autoptr (GError) java_err = g_error_new_literal (nuvashalauncher_JAVA_ERROR, 1, "java");
  g_assert_cmpstr (nuvashalauncher_error_message (java_err), ==, "The Java executable could not be used. Select a working Java runtime.");

  g_autoptr (GError) mc_err = g_error_new_literal (nuvashalauncher_MC_ERROR, 1, "mc");
  g_assert_cmpstr (nuvashalauncher_error_message (mc_err), ==, "The game metadata is invalid or unavailable. Refresh the version list and try again.");

  g_autoptr (GError) r_io = g_error_new_literal (nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_IO, "io");
  g_assert_cmpstr (nuvashalauncher_error_message (r_io), ==, "Could not read or write files. Check the location, permissions, and free disk space.");

  g_autoptr (GError) r_path = g_error_new_literal (nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_UNSAFE_PATH, "path");
  g_assert_cmpstr (nuvashalauncher_error_message (r_path), ==, "The archive is invalid or unsafe. Download it again from a trusted source.");

  g_autoptr (GError) im_used = g_error_new_literal (nuvashalauncher_INSTANCE_MANAGER_ERROR, 1, "the directory is already used");
  g_assert_cmpstr (nuvashalauncher_error_message (im_used), ==, "The destination already exists. Choose another location.");

  g_autoptr (GError) im_nf = g_error_new_literal (nuvashalauncher_INSTANCE_MANAGER_ERROR, nuvashalauncher_INSTANCE_MANAGER_ERROR_NOT_FOUND, "nf");
  g_assert_cmpstr (nuvashalauncher_error_message (im_nf), ==, "This instance is no longer available. Refresh the instance list.");

  g_autoptr (GError) launch_err = g_error_new_literal (nuvashalauncher_LAUNCH_ERROR, 1, "launch");
  g_assert_cmpstr (nuvashalauncher_error_message (launch_err), ==, "Your instance settings are invalid. Check the selected version and JVM arguments.");

  g_autoptr (GError) inst_err = g_error_new_literal (nuvashalauncher_INSTANCE_ERROR, 1, "inst");
  g_assert_cmpstr (nuvashalauncher_error_message (inst_err), ==, "Choose another instance name or directory.");

  g_autoptr (GError) sett_err = g_error_new_literal (nuvashalauncher_SETTINGS_ERROR, 1, "sett");
  g_assert_cmpstr (nuvashalauncher_error_message (sett_err), ==, "Could not read or write files. Check the location, permissions, and free disk space.");

  g_autoptr (GError) acc_io = g_error_new_literal (nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_IO, "accio");
  g_assert_cmpstr (nuvashalauncher_error_message (acc_io), ==, "Could not save the account. Check your system keyring and file permissions.");

  g_autoptr (GError) acc_inv = g_error_new_literal (nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_INVALID, "custom invalid msg");
  g_assert_cmpstr (nuvashalauncher_error_message (acc_inv), ==, "custom invalid msg");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/errors/auth-types", test_auth_errors_are_typed);
  g_test_add_func ("/errors/categories", test_error_categories);
  g_test_add_func ("/errors/exhaustive", test_error_domains_exhaustive);
  g_test_add_func ("/errors/widgets", test_error_widgets);
  return g_test_run ();
}
