/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-cli.h"

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-cache.h"
#include "nuvashalauncher-errors.h"
#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-nbt.h"
#include "nuvashalauncher-settings.h"

#include <json-glib/json-glib.h>

typedef struct {
  gboolean show_version;
  gboolean show_instances;
  gboolean show_accounts;
  gboolean json_output;
  gboolean headless;
  gboolean keep_files;
  gboolean clear_cache;
  char *import_path;
  char *launch_key;
  char *account_key;
  char *server_address;
  char *world_name;
  char *list_worlds;
  char *list_servers;
  char *switch_account;
  char *remove_account;
  char *add_offline_account;
  char *remove_instance;
  char *create_instance;
  char *mc_version;
  char *loader_name;
  char *loader_version;
  char *export_instance;
  char *output_path;
} CliOptions;

static void
cli_options_clear (CliOptions *opts)
{
  g_free (opts->import_path);
  g_free (opts->launch_key);
  g_free (opts->account_key);
  g_free (opts->server_address);
  g_free (opts->world_name);
  g_free (opts->list_worlds);
  g_free (opts->list_servers);
  g_free (opts->switch_account);
  g_free (opts->remove_account);
  g_free (opts->add_offline_account);
  g_free (opts->remove_instance);
  g_free (opts->create_instance);
  g_free (opts->mc_version);
  g_free (opts->loader_name);
  g_free (opts->loader_version);
  g_free (opts->export_instance);
  g_free (opts->output_path);
}

static GOptionContext *
build_option_context (CliOptions *opts)
{
  const GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &opts->show_version,
      "Show the version", NULL },
    { "import", 'i', 0, G_OPTION_ARG_FILENAME, &opts->import_path,
      "Import a local .mrpack", "FILE" },
    { "launch", 'l', 0, G_OPTION_ARG_STRING, &opts->launch_key,
      "Launch an instance by UUID or name", "INSTANCE" },
    { "headless", 0, 0, G_OPTION_ARG_NONE, &opts->headless,
      "Run --launch without opening a window", NULL },
    { "account", 0, 0, G_OPTION_ARG_STRING, &opts->account_key,
      "Use this account for --launch", "ACCOUNT" },
    { "server", 's', 0, G_OPTION_ARG_STRING, &opts->server_address,
      "Join a server directly when using --launch", "ADDRESS" },
    { "world", 0, 0, G_OPTION_ARG_STRING, &opts->world_name,
      "Join a singleplayer world directly when using --launch", "WORLD" },
    { "list-instances", 0, 0, G_OPTION_ARG_NONE, &opts->show_instances,
      "List instances", NULL },
    { "list-accounts", 0, 0, G_OPTION_ARG_NONE, &opts->show_accounts,
      "List accounts", NULL },
    { "list-worlds", 0, 0, G_OPTION_ARG_STRING, &opts->list_worlds,
      "List singleplayer worlds for an instance", "INSTANCE" },
    { "list-servers", 0, 0, G_OPTION_ARG_STRING, &opts->list_servers,
      "List saved servers for an instance", "INSTANCE" },
    { "json", 0, 0, G_OPTION_ARG_NONE, &opts->json_output,
      "Format --list-* output as JSON", NULL },
    { "switch-account", 0, 0, G_OPTION_ARG_STRING, &opts->switch_account,
      "Make an account active", "ACCOUNT" },
    { "remove-account", 0, 0, G_OPTION_ARG_STRING, &opts->remove_account,
      "Remove an account by UUID or name", "ACCOUNT" },
    { "add-offline-account", 0, 0, G_OPTION_ARG_STRING, &opts->add_offline_account,
      "Create and activate an offline account", "NAME" },
    { "create-instance", 0, 0, G_OPTION_ARG_STRING, &opts->create_instance,
      "Create a new instance", "NAME" },
    { "mc-version", 0, 0, G_OPTION_ARG_STRING, &opts->mc_version,
      "Minecraft version for --create-instance", "VERSION" },
    { "loader", 0, 0, G_OPTION_ARG_STRING, &opts->loader_name,
      "Mod loader for --create-instance (vanilla, fabric, forge, neoforge, quilt)",
      "LOADER" },
    { "loader-version", 0, 0, G_OPTION_ARG_STRING, &opts->loader_version,
      "Loader version for --create-instance", "VERSION" },
    { "remove-instance", 0, 0, G_OPTION_ARG_STRING, &opts->remove_instance,
      "Remove an instance by UUID or name", "INSTANCE" },
    { "keep-files", 0, 0, G_OPTION_ARG_NONE, &opts->keep_files,
      "Keep instance files on disk when using --remove-instance", NULL },
    { "export-instance", 0, 0, G_OPTION_ARG_STRING, &opts->export_instance,
      "Export an instance to a .mrpack archive", "INSTANCE" },
    { "output", 'o', 0, G_OPTION_ARG_FILENAME, &opts->output_path,
      "Output .mrpack path for --export-instance", "FILE" },
    { "clear-cache", 0, 0, G_OPTION_ARG_NONE, &opts->clear_cache,
      "Clear the global download cache", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
  };

  GOptionContext *ctx = g_option_context_new ("[FILE.mrpack]");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_set_help_enabled (ctx, FALSE);
  return ctx;
}

void
nuvashalauncher_cli_add_options (GApplication *app)
{
  const GOptionEntry entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, NULL, "Show the version", NULL },
    { "import", 'i', 0, G_OPTION_ARG_FILENAME, NULL, "Import a local .mrpack", "FILE" },
    { "launch", 'l', 0, G_OPTION_ARG_STRING, NULL, "Launch an instance by UUID or name", "INSTANCE" },
    { "headless", 0, 0, G_OPTION_ARG_NONE, NULL, "Run --launch without opening a window", NULL },
    { "account", 0, 0, G_OPTION_ARG_STRING, NULL, "Use this account for --launch", "ACCOUNT" },
    { "server", 's', 0, G_OPTION_ARG_STRING, NULL, "Join a server directly when using --launch", "ADDRESS" },
    { "world", 0, 0, G_OPTION_ARG_STRING, NULL, "Join a singleplayer world directly when using --launch", "WORLD" },
    { "list-instances", 0, 0, G_OPTION_ARG_NONE, NULL, "List instances", NULL },
    { "list-accounts", 0, 0, G_OPTION_ARG_NONE, NULL, "List accounts", NULL },
    { "list-worlds", 0, 0, G_OPTION_ARG_STRING, NULL, "List singleplayer worlds for an instance", "INSTANCE" },
    { "list-servers", 0, 0, G_OPTION_ARG_STRING, NULL, "List saved servers for an instance", "INSTANCE" },
    { "json", 0, 0, G_OPTION_ARG_NONE, NULL, "Format --list-* output as JSON", NULL },
    { "switch-account", 0, 0, G_OPTION_ARG_STRING, NULL, "Make an account active", "ACCOUNT" },
    { "remove-account", 0, 0, G_OPTION_ARG_STRING, NULL, "Remove an account by UUID or name", "ACCOUNT" },
    { "add-offline-account", 0, 0, G_OPTION_ARG_STRING, NULL, "Create and activate an offline account", "NAME" },
    { "create-instance", 0, 0, G_OPTION_ARG_STRING, NULL, "Create a new instance", "NAME" },
    { "mc-version", 0, 0, G_OPTION_ARG_STRING, NULL, "Minecraft version for --create-instance", "VERSION" },
    { "loader", 0, 0, G_OPTION_ARG_STRING, NULL, "Mod loader for --create-instance (vanilla, fabric, forge, neoforge, quilt)", "LOADER" },
    { "loader-version", 0, 0, G_OPTION_ARG_STRING, NULL, "Loader version for --create-instance", "VERSION" },
    { "remove-instance", 0, 0, G_OPTION_ARG_STRING, NULL, "Remove an instance by UUID or name", "INSTANCE" },
    { "keep-files", 0, 0, G_OPTION_ARG_NONE, NULL, "Keep instance files on disk when using --remove-instance", NULL },
    { "export-instance", 0, 0, G_OPTION_ARG_STRING, NULL, "Export an instance to a .mrpack archive", "INSTANCE" },
    { "output", 'o', 0, G_OPTION_ARG_FILENAME, NULL, "Output .mrpack path for --export-instance", "FILE" },
    { "clear-cache", 0, 0, G_OPTION_ARG_NONE, NULL, "Clear the global download cache", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
  };

  g_application_add_main_option_entries (app, entries);
}

static void
print_json_builder (JsonBuilder *builder)
{
  g_autoptr (JsonNode) root = json_builder_get_root (builder);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  json_generator_set_root (gen, root);
  g_autofree char *data = json_generator_to_data (gen, NULL);
  g_print ("%s\n", data);
}

static int
list_instances (gboolean json_output)
{
  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  GListModel *instances =
    G_LIST_MODEL (nuvashalauncher_instance_manager_get_instances (manager));
  guint count = g_list_model_get_n_items (instances);

  if (json_output)
    {
      g_autoptr (JsonBuilder) builder = json_builder_new ();
      json_builder_begin_array (builder);
      for (guint i = 0; i < count; i++)
        {
          g_autoptr (NuvashaLauncherInstance) instance =
            g_list_model_get_item (instances, i);
          const char *id = nuvashalauncher_instance_get_id (instance);
          g_autofree char *name = NULL;
          g_autofree char *mc_version = NULL;
          g_autofree char *loader_version = NULL;
          g_autofree char *directory = NULL;
          NuvashaLauncherLoader loader = NUVASHALAUNCHER_LOADER_VANILLA;

          g_object_get (instance,
                        "name", &name,
                        "minecraft-version", &mc_version,
                        "loader", &loader,
                        "loader-version", &loader_version,
                        "directory", &directory,
                        NULL);

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "id");
          json_builder_add_string_value (builder, id != NULL ? id : "");
          json_builder_set_member_name (builder, "name");
          json_builder_add_string_value (builder, name != NULL ? name : "");
          json_builder_set_member_name (builder, "minecraft_version");
          json_builder_add_string_value (builder, mc_version != NULL ? mc_version : "");
          json_builder_set_member_name (builder, "loader");
          json_builder_add_string_value (builder, nuvashalauncher_loader_to_string (loader));
          if (loader_version != NULL)
            {
              json_builder_set_member_name (builder, "loader_version");
              json_builder_add_string_value (builder, loader_version);
            }
          json_builder_set_member_name (builder, "directory");
          json_builder_add_string_value (builder, directory != NULL ? directory : "");
          json_builder_end_object (builder);
        }
      json_builder_end_array (builder);
      print_json_builder (builder);
      return 0;
    }

  g_print ("%-36s  %-24s  %-10s  %s\n", "UUID", "NAME", "VERSION", "LOADER");
  for (guint i = 0; i < count; i++)
    {
      g_autoptr (NuvashaLauncherInstance) instance =
        g_list_model_get_item (instances, i);
      const char *id = nuvashalauncher_instance_get_id (instance);
      g_autofree char *name = NULL;
      g_autofree char *mc_version = NULL;
      NuvashaLauncherLoader loader = NUVASHALAUNCHER_LOADER_VANILLA;

      g_object_get (instance,
                    "name", &name,
                    "minecraft-version", &mc_version,
                    "loader", &loader,
                    NULL);
      g_print ("%-36s  %-24s  %-10s  %s\n",
               id != NULL ? id : "",
               name != NULL ? name : "",
               mc_version != NULL ? mc_version : "",
               nuvashalauncher_loader_to_string (loader));
    }

  return 0;
}

static char *
find_instance_directory (const char *key)
{
  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return NULL;
    }

  NuvashaLauncherInstance *instance =
    nuvashalauncher_instance_manager_find (manager, key, &error);
  if (instance == NULL)
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return NULL;
    }

  char *directory = NULL;
  g_object_get (instance, "directory", &directory, NULL);
  return directory;
}

static int
list_worlds (const char *instance_key, gboolean json_output)
{
  g_autofree char *instance_dir = find_instance_directory (instance_key);
  if (instance_dir == NULL)
    return 1;

  g_autofree char *saves_dir = g_build_filename (instance_dir, "saves", NULL);
  g_autoptr (GPtrArray) folders = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);

  if (g_file_test (saves_dir, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) dir = g_dir_open (saves_dir, 0, NULL);
      const char *entry = NULL;

      while (dir != NULL && (entry = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *world_dir = g_build_filename (saves_dir, entry, NULL);
          g_autofree char *level_dat = g_build_filename (world_dir, "level.dat", NULL);
          if (!g_file_test (world_dir, G_FILE_TEST_IS_DIR) ||
              !g_file_test (level_dat, G_FILE_TEST_IS_REGULAR))
            continue;

          g_autoptr (NuvashaLauncherNbtTag) root =
            nuvashalauncher_nbt_parse_file (level_dat, NULL);
          NuvashaLauncherNbtTag *data =
            root != NULL ? nuvashalauncher_nbt_compound_get (root, "Data") : NULL;
          const char *level_name =
            data != NULL ? nuvashalauncher_nbt_compound_get_string (data, "LevelName") : NULL;

          g_ptr_array_add (folders, g_strdup (entry));
          g_ptr_array_add (names, g_strdup (level_name != NULL && *level_name != '\0'
                                            ? level_name : entry));
        }
    }

  if (json_output)
    {
      g_autoptr (JsonBuilder) builder = json_builder_new ();
      json_builder_begin_array (builder);
      for (guint i = 0; i < folders->len; i++)
        {
          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "folder");
          json_builder_add_string_value (builder, g_ptr_array_index (folders, i));
          json_builder_set_member_name (builder, "name");
          json_builder_add_string_value (builder, g_ptr_array_index (names, i));
          json_builder_end_object (builder);
        }
      json_builder_end_array (builder);
      print_json_builder (builder);
      return 0;
    }

  g_print ("%-32s  %s\n", "FOLDER", "NAME");
  for (guint i = 0; i < folders->len; i++)
    g_print ("%-32s  %s\n",
             (const char *) g_ptr_array_index (folders, i),
             (const char *) g_ptr_array_index (names, i));
  return 0;
}

static int
list_servers (const char *instance_key, gboolean json_output)
{
  g_autofree char *instance_dir = find_instance_directory (instance_key);
  if (instance_dir == NULL)
    return 1;

  g_autofree char *servers_dat = g_build_filename (instance_dir, "servers.dat", NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) ips = g_ptr_array_new_with_free_func (g_free);

  if (g_file_test (servers_dat, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (NuvashaLauncherNbtTag) root =
        nuvashalauncher_nbt_parse_file (servers_dat, NULL);
      NuvashaLauncherNbtTag *list =
        root != NULL ? nuvashalauncher_nbt_compound_get (root, "servers") : NULL;

      if (list != NULL && list->type == NUVASHALAUNCHER_NBT_LIST &&
          list->v_list.items != NULL)
        {
          for (guint i = 0; i < list->v_list.items->len; i++)
            {
              NuvashaLauncherNbtTag *item = g_ptr_array_index (list->v_list.items, i);
              if (item->type != NUVASHALAUNCHER_NBT_COMPOUND)
                continue;
              const char *name = nuvashalauncher_nbt_compound_get_string (item, "name");
              const char *ip = nuvashalauncher_nbt_compound_get_string (item, "ip");
              g_ptr_array_add (names, g_strdup (name != NULL ? name : ""));
              g_ptr_array_add (ips, g_strdup (ip != NULL ? ip : ""));
            }
        }
    }

  if (json_output)
    {
      g_autoptr (JsonBuilder) builder = json_builder_new ();
      json_builder_begin_array (builder);
      for (guint i = 0; i < ips->len; i++)
        {
          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "name");
          json_builder_add_string_value (builder, g_ptr_array_index (names, i));
          json_builder_set_member_name (builder, "ip");
          json_builder_add_string_value (builder, g_ptr_array_index (ips, i));
          json_builder_end_object (builder);
        }
      json_builder_end_array (builder);
      print_json_builder (builder);
      return 0;
    }

  g_print ("%-32s  %s\n", "NAME", "ADDRESS");
  for (guint i = 0; i < ips->len; i++)
    g_print ("%-32s  %s\n",
             (const char *) g_ptr_array_index (names, i),
             (const char *) g_ptr_array_index (ips, i));
  return 0;
}

static NuvashaLauncherAccount *
find_account (NuvashaLauncherAccounts *accounts, const char *key)
{
  for (guint i = 0; i < nuvashalauncher_accounts_count (accounts); i++)
    {
      NuvashaLauncherAccount *account =
        g_ptr_array_index (nuvashalauncher_accounts_list (accounts), i);
      if (g_strcmp0 (account->id, key) == 0 ||
          g_strcmp0 (account->uuid, key) == 0 ||
          g_strcmp0 (account->name, key) == 0)
        return account;
    }
  return NULL;
}

static int
list_accounts (gboolean json_output)
{
  g_autoptr (NuvashaLauncherAccounts) accounts = nuvashalauncher_accounts_new ();
  const char *active_id = nuvashalauncher_accounts_get_active_id (accounts);
  guint count = nuvashalauncher_accounts_count (accounts);

  if (json_output)
    {
      g_autoptr (JsonBuilder) builder = json_builder_new ();
      json_builder_begin_array (builder);
      for (guint i = 0; i < count; i++)
        {
          NuvashaLauncherAccount *account =
            g_ptr_array_index (nuvashalauncher_accounts_list (accounts), i);
          gboolean active = g_strcmp0 (account->id, active_id) == 0;

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "id");
          json_builder_add_string_value (builder, account->id != NULL ? account->id : "");
          json_builder_set_member_name (builder, "uuid");
          json_builder_add_string_value (builder, account->uuid != NULL ? account->uuid : "");
          json_builder_set_member_name (builder, "name");
          json_builder_add_string_value (builder, account->name != NULL ? account->name : "");
          json_builder_set_member_name (builder, "type");
          json_builder_add_string_value (
            builder,
            account->type == NUVASHALAUNCHER_ACCOUNT_MICROSOFT ? "microsoft" : "offline");
          json_builder_set_member_name (builder, "active");
          json_builder_add_boolean_value (builder, active);
          json_builder_end_object (builder);
        }
      json_builder_end_array (builder);
      print_json_builder (builder);
      return 0;
    }

  g_print ("%-36s  %-18s  %s\n", "UUID", "NAME", "TYPE");
  for (guint i = 0; i < count; i++)
    {
      NuvashaLauncherAccount *account =
        g_ptr_array_index (nuvashalauncher_accounts_list (accounts), i);
      g_print ("%-36s  %-18s  %s%s\n",
               account->uuid != NULL ? account->uuid : "",
               account->name != NULL ? account->name : "",
               nuvashalauncher_account_type_name (account->type),
               g_strcmp0 (account->id, active_id) == 0 ? "  *" : "");
    }
  return 0;
}

static int
change_account (const char *key, gboolean remove)
{
  g_autoptr (NuvashaLauncherAccounts) accounts = nuvashalauncher_accounts_new ();
  NuvashaLauncherAccount *account = find_account (accounts, key);
  g_autoptr (GError) error = NULL;

  if (account == NULL)
    {
      g_printerr ("no account matches \"%s\"\n", key);
      return 1;
    }

  if (remove ? !nuvashalauncher_accounts_remove (accounts, account->id, &error)
             : !nuvashalauncher_accounts_set_active (accounts, account->id, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }
  return 0;
}

static int
add_offline_account (const char *name)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherAccount) account =
    nuvashalauncher_account_new_offline (name, &error);
  if (account == NULL)
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  g_autoptr (NuvashaLauncherAccounts) accounts = nuvashalauncher_accounts_new ();
  if (!nuvashalauncher_accounts_has_microsoft (accounts))
    {
      g_printerr ("Add a Microsoft account first to create offline accounts.\n");
      return 1;
    }
  if (!nuvashalauncher_accounts_add (accounts, account, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  g_print ("%s  %s\n", account->uuid, account->name);
  return 0;
}

static int
create_instance (const char *name, const char *mc_version,
                 const char *loader_name, const char *loader_version)
{
  NuvashaLauncherLoader loader = NUVASHALAUNCHER_LOADER_VANILLA;
  if (loader_name != NULL &&
      !nuvashalauncher_loader_from_string (loader_name, &loader))
    {
      g_printerr ("unknown loader \"%s\" (expected vanilla, fabric, forge, neoforge, or quilt)\n",
                  loader_name);
      return 2;
    }

  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  g_autoptr (NuvashaLauncherInstance) instance =
    nuvashalauncher_instance_manager_create (manager, name, NULL, mc_version,
                                             loader, loader_version, &error);
  if (instance == NULL)
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  g_autofree char *created_name = NULL;
  g_object_get (instance, "name", &created_name, NULL);
  g_print ("%s  %s\n",
           nuvashalauncher_instance_get_id (instance),
           created_name != NULL ? created_name : "");
  return 0;
}

typedef struct {
  GMainLoop *loop;
  gpointer object;
  gboolean success;
} AsyncOpData;

static void
finish_delete (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
  AsyncOpData *data = user_data;
  g_autoptr (GError) error = NULL;
  data->success = nuvashalauncher_instance_manager_delete_finish (
    NUVASHALAUNCHER_INSTANCE_MANAGER (data->object), result, &error);
  if (error != NULL)
    g_printerr ("%s\n", nuvashalauncher_error_message (error));
  g_main_loop_quit (data->loop);
}

static int
remove_instance (const char *key, gboolean keep_files)
{
  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  NuvashaLauncherInstance *instance =
    nuvashalauncher_instance_manager_find (manager, key, &error);
  if (instance == NULL)
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  AsyncOpData data = { loop, manager, FALSE };
  NuvashaLauncherTaskScope *scope =
    nuvashalauncher_instance_manager_delete_async (manager, instance, !keep_files,
                                                   finish_delete, &data);
  g_main_loop_run (loop);
  if (scope != NULL)
    nuvashalauncher_task_scope_unref (scope);
  g_main_loop_unref (loop);
  return data.success ? 0 : 1;
}

static void
finish_export (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
  AsyncOpData *data = user_data;
  g_autoptr (GError) error = NULL;
  data->success = nuvashalauncher_modpack_export_finish (result, &error);
  if (error != NULL)
    g_printerr ("%s\n", nuvashalauncher_error_message (error));
  g_main_loop_quit (data->loop);
}

static int
export_instance (const char *key, const char *output_path)
{
  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  NuvashaLauncherInstance *instance =
    nuvashalauncher_instance_manager_find (manager, key, &error);
  if (instance == NULL)
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  AsyncOpData data = { loop, NULL, FALSE };
  NuvashaLauncherTaskScope *scope =
    nuvashalauncher_modpack_export_async (instance, output_path, NULL, NULL, NULL,
                                          FALSE, NULL, NULL, finish_export, &data);
  g_main_loop_run (loop);
  if (scope != NULL)
    nuvashalauncher_task_scope_unref (scope);
  g_main_loop_unref (loop);
  return data.success ? 0 : 1;
}

static void
finish_clear_cache (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
  AsyncOpData *data = user_data;
  g_autoptr (GError) error = NULL;
  data->success = g_task_propagate_boolean (G_TASK (result), &error);
  if (error != NULL)
    g_printerr ("%s\n", nuvashalauncher_error_message (error));
  g_main_loop_quit (data->loop);
}

static int
clear_cache (void)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  AsyncOpData data = { loop, NULL, FALSE };
  NuvashaLauncherTaskScope *scope =
    nuvashalauncher_cache_clear_all_async (finish_clear_cache, &data);
  g_main_loop_run (loop);
  if (scope != NULL)
    nuvashalauncher_task_scope_unref (scope);
  g_main_loop_unref (loop);
  return data.success ? 0 : 1;
}

typedef struct {
  GMainLoop *loop;
  int exit_code;
} HeadlessLaunchData;

static void
on_launch_output (NuvashaLauncher *launcher G_GNUC_UNUSED,
                  const char *line,
                  const char *tag,
                  gpointer user_data G_GNUC_UNUSED)
{
  if (g_strcmp0 (tag, "stderr") == 0)
    g_printerr ("%s\n", line);
  else
    g_print ("%s\n", line);
}

static void
on_launch_state (NuvashaLauncher *launcher,
                 NuvashaLauncherLaunchState state,
                 const char *detail,
                 gpointer user_data)
{
  HeadlessLaunchData *data = user_data;

  switch (state)
    {
    case NUVASHALAUNCHER_LAUNCH_STOPPED:
      data->exit_code = 0;
      g_main_loop_quit (data->loop);
      break;
    case NUVASHALAUNCHER_LAUNCH_CRASHED:
    case NUVASHALAUNCHER_LAUNCH_FAILED:
    case NUVASHALAUNCHER_LAUNCH_NEEDS_SIGN_IN:
      if (detail != NULL && *detail != '\0')
        g_printerr ("%s\n", detail);
      else
        g_printerr ("%s\n", nuvashalauncher_launch_state_to_string (state));
      data->exit_code = 1;
      g_main_loop_quit (data->loop);
      break;
    default:
      (void) launcher;
      break;
    }
}

static int
launch_headless (const char *key,
                 const char *account_key,
                 const char *world_name,
                 const char *server_address)
{
  g_autoptr (NuvashaLauncherInstanceManager) manager =
    nuvashalauncher_instance_manager_new (NULL);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_instance_manager_load (manager, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  NuvashaLauncherInstance *instance =
    nuvashalauncher_instance_manager_find (manager, key, &error);
  if (instance == NULL)
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  g_autoptr (NuvashaLauncherSettings) settings = nuvashalauncher_settings_new ();
  g_autoptr (NuvashaLauncherAccounts) accounts = nuvashalauncher_accounts_new ();
  g_autoptr (NuvashaLauncher) launcher =
    nuvashalauncher_launcher_new (instance, settings, accounts);

  if (account_key != NULL &&
      !nuvashalauncher_launcher_set_account (launcher, account_key, &error))
    {
      g_printerr ("%s\n", nuvashalauncher_error_message (error));
      return 1;
    }

  if (world_name != NULL || server_address != NULL)
    nuvashalauncher_launcher_set_quick_play (launcher, world_name, server_address);

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  HeadlessLaunchData data = { loop, 1 };
  g_signal_connect (launcher, "output", G_CALLBACK (on_launch_output), NULL);
  g_signal_connect (launcher, "state", G_CALLBACK (on_launch_state), &data);

  nuvashalauncher_launcher_start (launcher);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  return data.exit_code;
}

static gboolean
has_help_flag (int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
    {
      if (g_strcmp0 (argv[i], "--") == 0)
        break;
      if (g_strcmp0 (argv[i], "--help") == 0 ||
          g_strcmp0 (argv[i], "-h") == 0 ||
          g_strcmp0 (argv[i], "-?") == 0)
        return TRUE;
    }
  return FALSE;
}

gboolean
nuvashalauncher_cli_try_run (int *argc, char ***argv, int *exit_code)
{
  CliOptions opts = { 0 };
  g_autoptr (GOptionContext) ctx = build_option_context (&opts);
  g_autoptr (GError) error = NULL;

  if (has_help_flag (*argc, *argv))
    {
      g_autofree char *help = g_option_context_get_help (ctx, TRUE, NULL);
      g_print ("%s", help);
      *exit_code = 0;
      return TRUE;
    }

  g_auto (GStrv) argv_owned = g_strdupv (*argv);
  g_autofree char **argv_copy = g_memdup2 (argv_owned, (gsize) (*argc + 1) * sizeof (char *));
  int argc_copy = *argc;
  if (!g_option_context_parse (ctx, &argc_copy, &argv_copy, &error))
    {
      g_printerr ("%s\n", error->message);
      cli_options_clear (&opts);
      *exit_code = 2;
      return TRUE;
    }

  gboolean has_mrpack_arg = FALSE;
  for (int i = 1; i < argc_copy; i++)
    if (argv_copy[i][0] != '-' && g_str_has_suffix (argv_copy[i], ".mrpack"))
      {
        has_mrpack_arg = TRUE;
        break;
      }

  guint actions = (opts.show_version ? 1u : 0u) +
                  (opts.show_instances ? 1u : 0u) +
                  (opts.show_accounts ? 1u : 0u) +
                  (opts.list_worlds != NULL ? 1u : 0u) +
                  (opts.list_servers != NULL ? 1u : 0u) +
                  (opts.switch_account != NULL ? 1u : 0u) +
                  (opts.remove_account != NULL ? 1u : 0u) +
                  (opts.add_offline_account != NULL ? 1u : 0u) +
                  (opts.remove_instance != NULL ? 1u : 0u) +
                  (opts.create_instance != NULL ? 1u : 0u) +
                  (opts.export_instance != NULL ? 1u : 0u) +
                  (opts.clear_cache ? 1u : 0u) +
                  (opts.launch_key != NULL ? 1u : 0u) +
                  ((opts.import_path != NULL || has_mrpack_arg) ? 1u : 0u);

  if (actions > 1 ||
      (opts.account_key != NULL && opts.launch_key == NULL) ||
      ((opts.server_address != NULL || opts.world_name != NULL) && opts.launch_key == NULL) ||
      (opts.server_address != NULL && opts.world_name != NULL) ||
      (opts.headless && opts.launch_key == NULL) ||
      (opts.keep_files && opts.remove_instance == NULL) ||
      ((opts.mc_version != NULL || opts.loader_name != NULL || opts.loader_version != NULL) &&
       opts.create_instance == NULL) ||
      (opts.create_instance != NULL && (opts.mc_version == NULL || *opts.mc_version == '\0')) ||
      (opts.output_path != NULL && opts.export_instance == NULL) ||
      (opts.export_instance != NULL && (opts.output_path == NULL || *opts.output_path == '\0')) ||
      (opts.json_output && !opts.show_instances && !opts.show_accounts &&
       opts.list_worlds == NULL && opts.list_servers == NULL))
    {
      g_printerr ("invalid combination of options; run with --help for usage.\n");
      cli_options_clear (&opts);
      *exit_code = 2;
      return TRUE;
    }

  if (opts.show_version)
    {
      g_print ("Nuvasha Launcher %s\n", NUVASHALAUNCHER_VERSION);
      *exit_code = 0;
    }
  else if (opts.show_instances)
    *exit_code = list_instances (opts.json_output);
  else if (opts.show_accounts)
    *exit_code = list_accounts (opts.json_output);
  else if (opts.list_worlds != NULL)
    *exit_code = list_worlds (opts.list_worlds, opts.json_output);
  else if (opts.list_servers != NULL)
    *exit_code = list_servers (opts.list_servers, opts.json_output);
  else if (opts.switch_account != NULL)
    *exit_code = change_account (opts.switch_account, FALSE);
  else if (opts.remove_account != NULL)
    *exit_code = change_account (opts.remove_account, TRUE);
  else if (opts.add_offline_account != NULL)
    *exit_code = add_offline_account (opts.add_offline_account);
  else if (opts.create_instance != NULL)
    *exit_code = create_instance (opts.create_instance, opts.mc_version,
                                  opts.loader_name, opts.loader_version);
  else if (opts.remove_instance != NULL)
    *exit_code = remove_instance (opts.remove_instance, opts.keep_files);
  else if (opts.export_instance != NULL)
    *exit_code = export_instance (opts.export_instance, opts.output_path);
  else if (opts.clear_cache)
    *exit_code = clear_cache ();
  else if (opts.launch_key != NULL && opts.headless)
    *exit_code = launch_headless (opts.launch_key, opts.account_key,
                                  opts.world_name, opts.server_address);
  else
    {
      cli_options_clear (&opts);
      return FALSE;
    }

  cli_options_clear (&opts);
  return TRUE;
}
