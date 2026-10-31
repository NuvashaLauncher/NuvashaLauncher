#include "nuvashalauncher-users-popover-private.h"
#include "nuvashalauncher-secret-store.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>

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

static GtkWidget *
find_widget (GtkWidget *root, GType type)
{
  GtkWidget *child;

  if (G_TYPE_CHECK_INSTANCE_TYPE (root, type))
    return root;
  for (child = gtk_widget_get_first_child (root); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_widget (child, type);
      if (found != NULL)
        return found;
    }
  return NULL;
}

static GtkCheckButton *
find_slim_button (GtkWidget *root)
{
  GtkWidget *child;

  if (GTK_IS_CHECK_BUTTON (root) &&
      g_object_get_data (G_OBJECT (root), "nuvashalauncher-slim") != NULL)
    return GTK_CHECK_BUTTON (root);
  for (child = gtk_widget_get_first_child (root); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkCheckButton *found = find_slim_button (child);
      if (found != NULL)
        return found;
    }
  return NULL;
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
  g_autoptr (GtkWindow) window = NULL;
  GtkButton *button;

  accounts = nuvashalauncher_accounts_new_for_path (accounts_path);
  account = nuvashalauncher_account_new_offline ("SkinTest", &error);
  g_assert_no_error (error);
  if (!nuvashalauncher_accounts_add (accounts, account, &error))
    {
      g_test_skip ("Secret Service is unavailable in this environment");
      return;
    }
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
  while (gtk_widget_get_first_child (GTK_WIDGET (owner->list_box)) != NULL)
    gtk_list_box_remove (owner->list_box,
                         gtk_widget_get_first_child (GTK_WIDGET (owner->list_box)));
  drain ();
  gtk_window_destroy (g_steal_pointer (&window));
  drain ();
  g_clear_object (&owner);
  nuvashalauncher_secret_store_remove_account ("SkinTest", NULL);
  nuvashalauncher_secret_store_clear_active (NULL);
  g_assert_cmpuint (criticals, ==, 0);
}

static GtkWidget *
find_error_label (GtkWidget *widget, const char *diagnostic)
{
  if (GTK_IS_LABEL (widget) && gtk_widget_get_visible (widget) &&
      g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), diagnostic) == 0)
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_error_label (child, diagnostic);
      if (found != NULL)
        return found;
    }
  return NULL;
}

static void
test_missing_local_skin_cleans_upload_state (void)
{
  g_autoptr (nuvashalauncherAccounts) accounts = NULL;
  g_autoptr (nuvashalauncherAccount) account = NULL;
  g_autoptr (nuvashalauncherUsersPopover) owner = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("nuvashalauncher-skin-upload-XXXXXX", NULL);
  g_autofree char *accounts_path = g_build_filename (root, "accounts.json", NULL);
  g_autofree char *missing = g_build_filename (root, "missing.png", NULL);
  g_autofree char *uri = g_filename_to_uri (missing, NULL, NULL);
  g_autofree char *preview_path = g_build_filename (root, "preview.png", NULL);
  g_autofree char *preview_uri = g_filename_to_uri (preview_path, NULL, NULL);
  g_autoptr (GdkPixbuf) preview = NULL;
  g_autoptr (GtkImage) image = NULL;
  g_autoptr (GtkWindow) window = NULL;
  GtkButton *anchor;
  AdwDialog *dialog;
  GtkWidget *dialog_child;
  GtkListBox *skins;
  GtkCheckButton *slim;
  GtkListBoxRow *target = NULL;

  g_log_set_fatal_mask (NULL, G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
  g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

  accounts = nuvashalauncher_accounts_new_for_path (accounts_path);
  account = nuvashalauncher_account_new_offline ("SkinTest", &error);
  g_assert_no_error (error);
  account->avatar_url = g_strdup (preview_uri);
  preview = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
  gdk_pixbuf_fill (preview, 0xffffffff);
  g_assert_true (gdk_pixbuf_save (preview, preview_path, "png", &error, NULL));
  g_assert_no_error (error);
  if (!nuvashalauncher_accounts_add (accounts, account, &error))
    {
      g_test_skip ("Secret Service is unavailable in this environment");
      return;
    }
  g_assert_no_error (error);
  owner = g_object_ref_sink (nuvashalauncher_users_popover_new (accounts));
  g_hash_table_replace (owner->profile_cache, g_strdup (account->id),
                        g_strdup ("{}"));
  if (account->uuid != NULL)
    g_hash_table_replace (owner->profile_cache, g_strdup (account->uuid),
                          g_strdup ("{}"));
  image = GTK_IMAGE (g_object_ref_sink (gtk_image_new ()));
  anchor = GTK_BUTTON (gtk_button_new_with_label ("anchor"));
  window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_child (window, GTK_WIDGET (anchor));
  gtk_widget_set_opacity (GTK_WIDGET (window), 0.0);
  gtk_widget_set_focusable (GTK_WIDGET (window), FALSE);
  gtk_window_present (window);
  drain ();
  dialog = nuvashalauncher_users_skin_menu_new (owner, image, account);
  account->access_token = g_strdup ("local-test-token");
  account->expires_at = G_MAXINT64;
  adw_dialog_present (dialog, GTK_WIDGET (anchor));
  drain ();
  dialog_child = adw_dialog_get_child (dialog);
  drain ();
  skins = GTK_LIST_BOX (find_widget (dialog_child, GTK_TYPE_LIST_BOX));
  slim = find_slim_button (dialog_child);
  g_assert_nonnull (skins);
  g_assert_nonnull (slim);

  for (GtkListBoxRow *row = gtk_list_box_get_row_at_index (skins, 0);
       row != NULL; row = gtk_list_box_get_row_at_index (skins,
                                                         gtk_list_box_row_get_index (row) + 1))
    if (g_strcmp0 (g_object_get_data (G_OBJECT (row),
                                      "nuvashalauncher-skin-url"), preview_uri) == 0)
      {
        target = row;
        break;
      }
  g_assert_nonnull (target);
  gtk_list_box_select_row (skins, target);
  g_object_set_data_full (G_OBJECT (target), "nuvashalauncher-skin-url",
                          g_strdup (uri), g_free);
  gtk_check_button_set_active (slim, TRUE);

  while (nuvashalauncher_users_skin_background_work_active ())
    g_main_context_iteration (NULL, TRUE);
  GtkWidget *status = find_error_label (dialog_child,
                                        "The file or folder was not found. Check its location.");
  g_assert_nonnull (status);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (status)), ==,
                   "The file or folder was not found. Check its location.");

  g_assert_false (nuvashalauncher_users_skin_background_work_active ());
  g_assert_true (adw_dialog_close (dialog));
  drain ();
  gtk_window_destroy (g_steal_pointer (&window));
  drain ();
  nuvashalauncher_secret_store_remove_account ("SkinTest", NULL);
  nuvashalauncher_secret_store_clear_active (NULL);
  g_remove (accounts_path);
  g_remove (preview_path);
  g_rmdir (root);
}

int
main (int argc, char **argv)
{
  g_setenv ("NUVASHA_TEST_SECRET_STORE", "1", TRUE);
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_log_set_writer_func (count_criticals, NULL, NULL);
  g_test_add_func ("/skin-dialog/close", test_skin_dialog_close);
  g_test_add_func ("/skin-dialog/missing-local-upload",
                   test_missing_local_skin_cleans_upload_state);
  return g_test_run ();
}
