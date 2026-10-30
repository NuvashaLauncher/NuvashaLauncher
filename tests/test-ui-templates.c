#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include "nuvashalauncher-create-instance-dialog.h"
#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-offline-account-dialog.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-sign-in-dialog.h"
#include "nuvashalauncher-filter-entry.h"

static void
test_templates_are_bundled (void)
{
  const char *directory = "/com/github/nuvashalauncher/nuvashalauncher/ui";
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) names = g_resources_enumerate_children (
    directory, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);

  g_assert_no_error (error);
  g_assert_nonnull (names);

  for (guint i = 0; names[i] != NULL; i++)
    {
      g_autofree char *path = g_build_filename (directory, names[i], NULL);
      g_autoptr (GBytes) bytes = NULL;

      g_assert_true (g_str_has_suffix (names[i], ".ui"));
      bytes = g_resources_lookup_data (path, G_RESOURCE_LOOKUP_FLAGS_NONE,
                                       &error);
      g_assert_no_error (error);
      g_assert_nonnull (bytes);
      g_assert_cmpuint (g_bytes_get_size (bytes), >, 0);
    }

  /* The resource directory must contain the complete current UI inventory,
   * not just an empty directory that happens to enumerate successfully. */
  g_assert_cmpuint (g_strv_length (names), >, 0);
}

static void
test_builder_templates_parse (void)
{
  g_type_ensure (nuvashalauncher_TYPE_FILTER_ENTRY);
  static const char *paths[] = {
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-import.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-main-shell.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-browser-toolbar.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-browser-states.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-details-toolbar.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-details-body.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-optional.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-error.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-content-result-row.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-content-file-row.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-download-row.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-skin-row.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-downloads-popover.ui",
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-content-details.ui",
  };

  for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (GtkBuilder) builder =
        gtk_builder_new_from_resource (paths[i]);

      g_assert_no_error (error);
      g_assert_nonnull (builder);
    }
}

static void
test_stylesheet_parses (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (
    provider, "/com/github/nuvashalauncher/nuvashalauncher/nuvashalauncher.css");
  g_assert_true (GTK_IS_CSS_PROVIDER (provider));

  g_object_unref (provider);
}

static void
test_custom_templates_instantiate (void)
{
  g_autoptr (nuvashalauncherOfflineAccountDialog) offline =
    g_object_ref_sink (nuvashalauncher_offline_account_dialog_new ());
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (nuvashalauncherSettings) settings = nuvashalauncher_settings_new ();
  g_autoptr (nuvashalauncherCreateInstanceDialog) create =
    g_object_ref_sink (nuvashalauncher_create_instance_dialog_new (manager, settings));
  g_autoptr (nuvashalauncherSignInDialog) sign_in =
    g_object_ref_sink (nuvashalauncher_sign_in_dialog_new ());

  g_assert_true (nuvashalauncher_IS_OFFLINE_ACCOUNT_DIALOG (offline));
  g_assert_true (nuvashalauncher_IS_CREATE_INSTANCE_DIALOG (create));
  g_assert_true (nuvashalauncher_IS_SIGN_IN_DIALOG (sign_in));
  g_assert_true (GTK_IS_WIDGET (offline));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_test_add_func ("/ui/templates-are-bundled", test_templates_are_bundled);
  g_test_add_func ("/ui/templates-parse", test_builder_templates_parse);
  g_test_add_func ("/ui/stylesheet-parses", test_stylesheet_parses);
  g_test_add_func ("/ui/custom-templates-instantiate",
                   test_custom_templates_instantiate);
  return g_test_run ();
}
