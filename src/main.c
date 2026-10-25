/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvasha, Nuvasha Launcher. GTK chooses the available display
 * backend at startup. */

#include <adwaita.h>
#include <gtk/gtk.h>

#include "nuvasha-instance-manager.h"
#include "nuvasha-i18n.h"
#include "nuvasha-main-window.h"
#include "nuvasha-download.h"
#include "nuvasha-settings.h"

static void
on_quit (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data)
{
  g_application_quit (G_APPLICATION (user_data));
}

static const GActionEntry app_entries[] = {
  { .name = "quit", .activate = on_quit },
};

static int
list_instances (void)
{
  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;
  GListModel *instances;

  if (!nuvasha_instance_manager_load (manager, &error))
    {
      g_printerr ("cannot read the instance list: %s\n", error->message);
      return 1;
    }

  instances = G_LIST_MODEL (nuvasha_instance_manager_get_instances (manager));
  g_print ("%-36s  %s\n", "UUID", "NAME");

  for (guint i = 0; i < g_list_model_get_n_items (instances); i++)
    {
      g_autoptr (NuvashaInstance) instance =
        g_list_model_get_item (instances, i);
      const char *id = nuvasha_instance_get_id (instance);
      g_autofree char *name = NULL;

      g_object_get (instance, "name", &name, NULL);
      g_print ("%-36s  %s\n", id != NULL ? id : "", name != NULL ? name : "");
    }

  return 0;
}

static void
load_application_css (void)
{
  g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (
    provider, "/com/github/nuvashalauncher/nuvasha/nuvasha.css");
  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void
on_activate (GApplication *app, gpointer user_data G_GNUC_UNUSED)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (window == NULL)
    {
      GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
      if (windows != NULL)
        window = GTK_WINDOW (windows->data);
    }

  /* Reuse the existing application window on subsequent activations. */
  if (window != NULL)
    {
      gtk_window_present (window);
      return;
    }

  g_autoptr (NuvashaInstanceManager) manager = nuvasha_instance_manager_new (NULL);
  g_autoptr (NuvashaSettings) settings = nuvasha_settings_new ();
  g_autoptr (GError) error = NULL;

  nuvasha_i18n_init (nuvasha_settings_get_language (settings));

  if (!nuvasha_instance_manager_load (manager, &error))
    g_warning ("cannot read the instance list: %s", error->message);

  NuvashaMainWindow *main_window =
    nuvasha_main_window_new (ADW_APPLICATION (app), manager);

  gtk_window_present (GTK_WINDOW (main_window));
}

static NuvashaMainWindow *
ensure_main_window (GApplication *app, gboolean present)
{
  GtkWindow *existing =
    gtk_application_get_active_window (GTK_APPLICATION (app));

  if (existing == NULL)
    {
      GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
      if (windows != NULL)
        existing = GTK_WINDOW (windows->data);
    }

  if (existing != NULL)
    {
      if (present)
        gtk_window_present (existing);
      return nuvasha_MAIN_WINDOW (existing);
    }

  g_autoptr (NuvashaInstanceManager) manager =
    nuvasha_instance_manager_new (NULL);
  g_autoptr (NuvashaSettings) settings = nuvasha_settings_new ();
  g_autoptr (GError) error = NULL;

  nuvasha_i18n_init (nuvasha_settings_get_language (settings));

  if (!nuvasha_instance_manager_load (manager, &error))
    g_warning ("cannot read the instance list: %s", error->message);

  NuvashaMainWindow *window =
    nuvasha_main_window_new (ADW_APPLICATION (app), manager);

  if (present)
    gtk_window_present (GTK_WINDOW (window));

  return window;
}

static int
on_command_line (GApplication *app, GApplicationCommandLine *command_line,
                 gpointer user_data G_GNUC_UNUSED)
{
  GVariantDict *options =
    g_application_command_line_get_options_dict (command_line);
  const char *import_path = NULL;
  const char *file_path = NULL;
  const char *launch_id = NULL;
  gboolean show_version = FALSE;
  gboolean show_instances = FALSE;
  g_auto (GStrv) arguments = NULL;
  int argument_count = 0;
  g_autoptr (GError) error = NULL;

  g_variant_dict_lookup (options, "import", "&s", &import_path);
  g_variant_dict_lookup (options, "launch", "&s", &launch_id);
  g_variant_dict_lookup (options, "version", "b", &show_version);
  g_variant_dict_lookup (options, "list-instances", "b", &show_instances);

  arguments = g_application_command_line_get_arguments (command_line,
                                                        &argument_count);
  for (int i = 1; i < argument_count; i++)
    if (arguments[i][0] != '-' && g_str_has_suffix (arguments[i], ".mrpack"))
      {
        file_path = arguments[i];
        break;
      }

  if (show_version)
    {
      g_application_command_line_print (command_line, "NuvashaLauncher %s\n",
                                        nuvasha_VERSION);
      return 0;
    }

  if (show_instances)
    return list_instances ();

  if (launch_id != NULL && (import_path != NULL || file_path != NULL))
    {
      g_application_command_line_printerr (
        command_line, "--launch and --import cannot be used together.\n");
      return 2;
    }

  if (launch_id != NULL)
    {
      /* Keep the application window registered while launch work is active,
       * without presenting it. */
      NuvashaMainWindow *window = ensure_main_window (app, FALSE);

      if (!nuvasha_main_window_launch_instance (window, launch_id, &error))
        {
          g_application_command_line_printerr (command_line, "%s\n",
                                                error->message);
          gtk_window_present (GTK_WINDOW (window));
          return 1;
        }

      return 0;
    }

  if (import_path != NULL)
    {
      NuvashaMainWindow *window = ensure_main_window (app, TRUE);
      nuvasha_main_window_import_file (window, import_path);
      return 0;
    }

  if (file_path != NULL)
    {
      NuvashaMainWindow *window = ensure_main_window (app, TRUE);
      nuvasha_main_window_import_file (window, file_path);
      return 0;
    }

  g_application_activate (app);
  return 0;
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = NULL;

  if (argc == 2 && g_strcmp0 (argv[1], "--version") == 0)
    {
      g_print ("NuvashaLauncher %s\n", nuvasha_VERSION);
      return 0;
    }

  if (argc == 2 && g_strcmp0 (argv[1], "--list-instances") == 0)
    return list_instances ();

  if (!gtk_init_check ())
    {
      g_printerr ("nuvasha could not open a display.\n");
      return 1;
    }

  g_set_application_name ("Nuvasha Launcher");
  load_application_css ();

  app = adw_application_new ("com.github.nuvashalauncher.nuvasha",
                             G_APPLICATION_HANDLES_COMMAND_LINE);
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_entries,
                                   G_N_ELEMENTS (app_entries), app);
  g_application_add_main_option (G_APPLICATION (app), "version", 0, 0,
                                 G_OPTION_ARG_NONE, "Show the version", NULL);
  g_application_add_main_option (G_APPLICATION (app), "import", 'i', 0,
                                 G_OPTION_ARG_FILENAME, "Import a local .mrpack",
                                 "FILE");
  g_application_add_main_option (G_APPLICATION (app), "launch", 'l', 0,
                                 G_OPTION_ARG_STRING, "Launch an instance by ID",
                                 "INSTANCE_ID");
  g_application_add_main_option (G_APPLICATION (app), "list-instances", 0, 0,
                                 G_OPTION_ARG_NONE, "List instance IDs and names",
                                 NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "command-line", G_CALLBACK (on_command_line), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  return status;
}
