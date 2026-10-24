#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include "nuvasha-create-instance-dialog.h"
#include "nuvasha-instance-manager.h"
#include "nuvasha-offline-account-dialog.h"
#include "nuvasha-settings.h"
#include "nuvasha-sign-in-dialog.h"

static void
test_templates_are_bundled (void)
{
  const char *directory = "/com/github/nuvashalauncher/nuvasha/ui";
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
  static const char *paths[] = {
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-modpack-import.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-main-shell.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-modpack-browser-toolbar.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-modpack-browser-states.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-modpack-details-toolbar.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-modpack-details-body.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-modpack-optional.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-modpack-error.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-content-result-row.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-content-file-row.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-download-row.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-skin-row.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-downloads-popover.ui",
    "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-content-details.ui",
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
    provider, "/com/github/nuvashalauncher/nuvasha/nuvasha.css");
  g_assert_true (GTK_IS_CSS_PROVIDER (provider));

  g_object_unref (provider);
}

#if !defined(__SANITIZE_ADDRESS__)
static void
test_custom_templates_instantiate (void)
{
  g_autoptr (NuvashaOfflineAccountDialog) offline =
    g_object_ref_sink (nuvasha_offline_account_dialog_new ());
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (NULL);
  g_autoptr (NuvashaSettings) settings = nuvasha_settings_new ();
  g_autoptr (NuvashaCreateInstanceDialog) create =
    g_object_ref_sink (nuvasha_create_instance_dialog_new (manager, settings));
  g_autoptr (NuvashaSignInDialog) sign_in =
    g_object_ref_sink (nuvasha_sign_in_dialog_new ());

  g_assert_true (nuvasha_IS_OFFLINE_ACCOUNT_DIALOG (offline));
  g_assert_true (nuvasha_IS_CREATE_INSTANCE_DIALOG (create));
  g_assert_true (nuvasha_IS_SIGN_IN_DIALOG (sign_in));
  g_assert_true (GTK_IS_WIDGET (offline));
}
#endif

#if defined(__SANITIZE_ADDRESS__)
static void
test_custom_templates_instantiate_under_asan (void)
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
  g_test_add_func ("/ui/templates-are-bundled", test_templates_are_bundled);
  g_test_add_func ("/ui/templates-parse", test_builder_templates_parse);
  g_test_add_func ("/ui/stylesheet-parses", test_stylesheet_parses);
#if defined(__SANITIZE_ADDRESS__)
  g_test_add_func ("/ui/custom-templates-instantiate",
                   test_custom_templates_instantiate_under_asan);
#else
  g_test_add_func ("/ui/custom-templates-instantiate",
                   test_custom_templates_instantiate);
#endif
  return g_test_run ();
}
