/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncher, nuvashalauncher. GTK chooses the available display
 * backend at startup. */

#include <adwaita.h>
#include <gtk/gtk.h>

#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-main-window.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-accounts.h"

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

typedef struct {
  GMainLoop *loop;
  nuvashalauncherInstanceManager *manager;
  gboolean *success;
} DeleteData;

static void
finish_delete (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
  DeleteData *data = user_data;
  g_autoptr (GError) error = NULL;
  *data->success = nuvashalauncher_instance_manager_delete_finish (data->manager, result, &error);
  if (error != NULL)
    g_printerr ("%s\n", error->message);
  g_main_loop_quit (data->loop);
}

static int
list_instances (void)
{
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;
  GListModel *instances;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    {
      g_printerr ("cannot read the instance list: %s\n", error->message);
      return 1;
    }

  instances = G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (manager));
  g_print ("%-36s  %s\n", "UUID", "NAME");

  for (guint i = 0; i < g_list_model_get_n_items (instances); i++)
    {
      g_autoptr (nuvashalauncherInstance) instance =
        g_list_model_get_item (instances, i);
      const char *id = nuvashalauncher_instance_get_id (instance);
      g_autofree char *name = NULL;

      g_object_get (instance, "name", &name, NULL);
      g_print ("%-36s  %s\n", id != NULL ? id : "", name != NULL ? name : "");
    }

  return 0;
}

static nuvashalauncherAccount *
find_account (nuvashalauncherAccounts *accounts, const char *key)
{
  for (guint i = 0; i < nuvashalauncher_accounts_count (accounts); i++)
    {
      nuvashalauncherAccount *account = g_ptr_array_index (nuvashalauncher_accounts_list (accounts), i);
      if (g_strcmp0 (account->id, key) == 0 || g_strcmp0 (account->uuid, key) == 0 ||
          g_strcmp0 (account->name, key) == 0)
        return account;
    }
  return NULL;
}

static int
list_accounts (void)
{
  g_autoptr (nuvashalauncherAccounts) accounts = nuvashalauncher_accounts_new ();
  g_print ("%-36s  %s\n", "UUID", "NAME");
  for (guint i = 0; i < nuvashalauncher_accounts_count (accounts); i++)
    {
      nuvashalauncherAccount *account = g_ptr_array_index (nuvashalauncher_accounts_list (accounts), i);
      g_print ("%-36s  %s%s\n", account->uuid, account->name,
               g_strcmp0 (account->id, nuvashalauncher_accounts_get_active_id (accounts)) == 0
                 ? "  *" : "");
    }
  return 0;
}

static int
change_account (const char *key, gboolean remove)
{
  g_autoptr (nuvashalauncherAccounts) accounts = nuvashalauncher_accounts_new ();
  nuvashalauncherAccount *account = find_account (accounts, key);
  g_autoptr (GError) error = NULL;
  if (account == NULL)
    {
      g_printerr ("no account matches \"%s\"\n", key);
      return 1;
    }
  if (remove ? !nuvashalauncher_accounts_remove (accounts, account->id, &error)
             : !nuvashalauncher_accounts_set_active (accounts, account->id, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  return 0;
}

static int
remove_instance (const char *id)
{
  g_autoptr (nuvashalauncherInstanceManager) manager = nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;
  nuvashalauncherInstance *instance;
  GMainLoop *loop;
  gboolean success = FALSE;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    { g_printerr ("%s\n", error->message); return 1; }
  instance = nuvashalauncher_instance_manager_get_by_id (manager, id);
  if (instance == NULL)
    { g_printerr ("no instance has ID \"%s\"\n", id); return 1; }

  loop = g_main_loop_new (NULL, FALSE);
  DeleteData data = { loop, manager, &success };
  nuvashalauncher_instance_manager_delete_async (manager, instance, TRUE,
                                          finish_delete, &data);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  return success ? 0 : 1;
}

static void
load_application_css (void)
{
  g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (
    provider, "/com/github/nuvashalauncher/nuvashalauncher/nuvashalauncher.css");
  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static nuvashalauncherMainWindow *
create_main_window (GApplication *app, gboolean present)
{
  g_autoptr (nuvashalauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (nuvashalauncherSettings) settings = nuvashalauncher_settings_new ();
  g_autoptr (GError) error = NULL;
  nuvashalauncherMainWindow *window;

  nuvashalauncher_i18n_init (nuvashalauncher_settings_get_language (settings));

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    g_warning ("cannot read the instance list: %s", error->message);

  window = nuvashalauncher_main_window_new (ADW_APPLICATION (app), manager);
  if (present)
    gtk_window_present (GTK_WINDOW (window));

  return window;
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

  create_main_window (app, TRUE);
}

static nuvashalauncherMainWindow *
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
      return nuvashalauncher_MAIN_WINDOW (existing);
    }

  return create_main_window (app, present);
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
  const char *account_id = NULL;
  gboolean show_version = FALSE;
  gboolean show_instances = FALSE;
  gboolean show_accounts = FALSE;
  const char *switch_account = NULL;
  const char *remove_account_key = NULL;
  const char *remove_instance_id = NULL;
  g_auto (GStrv) arguments = NULL;
  int argument_count = 0;
  g_autoptr (GError) error = NULL;

  g_variant_dict_lookup (options, "import", "&s", &import_path);
  g_variant_dict_lookup (options, "launch", "&s", &launch_id);
  g_variant_dict_lookup (options, "version", "b", &show_version);
  g_variant_dict_lookup (options, "list-instances", "b", &show_instances);
  g_variant_dict_lookup (options, "list-accounts", "b", &show_accounts);
  g_variant_dict_lookup (options, "switch-account", "&s", &switch_account);
  g_variant_dict_lookup (options, "remove-account", "&s", &remove_account_key);
  g_variant_dict_lookup (options, "remove-instance", "&s", &remove_instance_id);
  g_variant_dict_lookup (options, "account", "&s", &account_id);

  arguments = g_application_command_line_get_arguments (command_line,
                                                        &argument_count);
  for (int i = 1; i < argument_count; i++)
    if (arguments[i][0] != '-' && g_str_has_suffix (arguments[i], ".mrpack"))
      {
        file_path = arguments[i];
        break;
      }

  {
    guint actions = (show_version || show_instances || show_accounts) +
                    (switch_account != NULL) + (remove_account_key != NULL) +
                    (remove_instance_id != NULL) + (launch_id != NULL) +
                    (import_path != NULL || file_path != NULL);

    if (actions > 1 || (account_id != NULL && launch_id == NULL))
      {
        g_application_command_line_printerr (
          command_line,
          "choose one command at a time; --account requires --launch INSTANCE_ID.\n");
        return 2;
      }
  }

  if (show_version)
    {
      g_application_command_line_print (command_line, "nuvashalauncher %s\n",
                                        nuvashalauncher_VERSION);
      return 0;
    }

  if (show_instances)
    return list_instances ();
  if (show_accounts)
    return list_accounts ();
  if (switch_account != NULL)
    return change_account (switch_account, FALSE);
  if (remove_account_key != NULL)
    return change_account (remove_account_key, TRUE);
  if (remove_instance_id != NULL)
    return remove_instance (remove_instance_id);

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
      nuvashalauncherMainWindow *window = ensure_main_window (app, FALSE);

       if (!nuvashalauncher_main_window_launch_instance_as (window, launch_id, account_id, &error))
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
      nuvashalauncherMainWindow *window = ensure_main_window (app, TRUE);
      nuvashalauncher_main_window_import_file (window, import_path);
      return 0;
    }

  if (file_path != NULL)
    {
      nuvashalauncherMainWindow *window = ensure_main_window (app, TRUE);
      nuvashalauncher_main_window_import_file (window, file_path);
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
      g_print ("nuvashalauncher %s\n", nuvashalauncher_VERSION);
      return 0;
    }

  if (argc == 2 && g_strcmp0 (argv[1], "--list-instances") == 0)
    return list_instances ();

  if (argc == 2 && g_strcmp0 (argv[1], "--list-accounts") == 0)
    return list_accounts ();

  if (argc == 3 && g_strcmp0 (argv[1], "--switch-account") == 0)
    return change_account (argv[2], FALSE);
  if (argc == 3 && g_strcmp0 (argv[1], "--remove-account") == 0)
    return change_account (argv[2], TRUE);
  if (argc == 3 && g_strcmp0 (argv[1], "--remove-instance") == 0)
    return remove_instance (argv[2]);

  if (!gtk_init_check ())
    {
      g_printerr ("nuvashalauncher could not open a display.\n");
      return 1;
    }

  g_set_application_name ("Nuvasha Launcher");
  load_application_css ();

  app = adw_application_new ("com.github.nuvashalauncher.nuvashalauncher",
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
                                  G_OPTION_ARG_NONE, "List instance UUIDs and names",
                                  NULL);
  g_application_add_main_option (G_APPLICATION (app), "list-accounts", 0, 0,
                                  G_OPTION_ARG_NONE, "List account UUIDs and names", NULL);
  g_application_add_main_option (G_APPLICATION (app), "switch-account", 0, 0,
                                 G_OPTION_ARG_STRING, "Make an account active", "ACCOUNT");
  g_application_add_main_option (G_APPLICATION (app), "remove-account", 0, 0,
                                 G_OPTION_ARG_STRING, "Remove an account by UUID or name", "ACCOUNT");
  g_application_add_main_option (G_APPLICATION (app), "remove-instance", 0, 0,
                                 G_OPTION_ARG_STRING, "Remove an instance by UUID", "INSTANCE_ID");
  g_application_add_main_option (G_APPLICATION (app), "account", 0, 0,
                                 G_OPTION_ARG_STRING, "Use this account for the launch", "ACCOUNT");
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "command-line", G_CALLBACK (on_command_line), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  return status;
}
