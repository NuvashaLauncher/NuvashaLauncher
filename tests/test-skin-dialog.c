#include "nuvashalauncher-users-popover-private.h"

#include <adwaita.h>
#include <gtk/gtk.h>

static guint criticals;

static GLogWriterOutput
count_criticals (GLogLevelFlags log_level, const GLogField *fields,
                 gsize n_fields, gpointer user_data)
{
  (void) fields; (void) n_fields; (void) user_data;
  if (log_level & G_LOG_LEVEL_CRITICAL)
    criticals++;
  return g_log_writer_default (log_level, fields, n_fields, NULL);
}

static void
drain (void)
{
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);
}

static void
test_skin_dialog_close (void)
{
  g_autoptr (nuvashalauncherAccounts) accounts = NULL;
  g_autoptr (nuvashalauncherAccount) account = NULL;
  g_autoptr (nuvashalauncherUsersPopover) owner = NULL;
  g_autoptr (GError) error = NULL;
  AdwDialog *dialog = NULL;
  g_autoptr (GtkImage) image = NULL;
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-skin-dialog-XXXXXX", NULL);
  g_autofree char *accounts_path = g_build_filename (root, "accounts.json", NULL);
  GtkWindow *window;
  GtkButton *button;

  accounts = nuvashalauncher_accounts_new_for_path (accounts_path);
  account = nuvashalauncher_account_new_offline ("SkinTest", &error);
  g_assert_no_error (error);
  g_assert_true (nuvashalauncher_accounts_add (accounts, account, &error));
  g_assert_no_error (error);
  owner = g_object_ref_sink (nuvashalauncher_users_popover_new (accounts));
  button = GTK_BUTTON (gtk_button_new_with_label ("anchor"));
  image = GTK_IMAGE (g_object_ref_sink (gtk_image_new ()));
  window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_child (window, GTK_WIDGET (button));
  gtk_widget_set_opacity (GTK_WIDGET (window), 0.0);
  gtk_widget_set_focusable (GTK_WIDGET (window), FALSE);
  gtk_window_present (window);
  drain ();

  dialog = nuvashalauncher_users_skin_menu_new (owner, image, account);
  adw_dialog_present (dialog, GTK_WIDGET (button));
  drain ();
  g_assert_true (adw_dialog_close (dialog));
  drain ();
  gtk_window_destroy (window);
  drain ();
  g_assert_cmpuint (criticals, ==, 0);
}

#if defined(__SANITIZE_ADDRESS__)
static void
test_skin_dialog_close_under_asan (void)
{
  g_test_skip ("GTK/Pango display caches are outside ASan ownership");
}
#endif

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_log_set_writer_func (count_criticals, NULL, NULL);
#if defined(__SANITIZE_ADDRESS__)
  g_test_add_func ("/skin-dialog/close", test_skin_dialog_close_under_asan);
#else
  g_test_add_func ("/skin-dialog/close", test_skin_dialog_close);
#endif
  return g_test_run ();
}
