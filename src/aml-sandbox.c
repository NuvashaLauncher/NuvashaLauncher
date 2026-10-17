/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-sandbox.h"
#include "aml-sandbox-private.h"
#include "aml-sandbox-display.h"
#include "aml-sandbox-mounts.h"
#include "aml-open-broker.h"

#include <unistd.h>


void
aml_sandbox_add_argument (GPtrArray *arguments, const char *value)
{
  g_ptr_array_add (arguments, g_strdup (value));
}

void
aml_sandbox_add_directory (GPtrArray *arguments, const char *path)
{
  aml_sandbox_add_argument (arguments, "--dir");
  aml_sandbox_add_argument (arguments, path);
}

void
aml_sandbox_bind_readonly_if_present (GPtrArray *arguments, const char *path)
{
  if (path != NULL && *path != '\0' && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      aml_sandbox_add_argument (arguments, "--ro-bind-try");
      aml_sandbox_add_argument (arguments, path);
      aml_sandbox_add_argument (arguments, path);
    }
}

static void
add_pair (GPtrArray *arguments, const char *option, const char *value)
{
  aml_sandbox_add_argument (arguments, option);
  aml_sandbox_add_argument (arguments, value);
}

static void
add_environment (GPtrArray *arguments, const char *name, const char *value)
{
  aml_sandbox_add_argument (arguments, "--setenv");
  aml_sandbox_add_argument (arguments, name);
  aml_sandbox_add_argument (arguments, value);
}

static void
unset_environment (GPtrArray *arguments, const char *name)
{
  aml_sandbox_add_argument (arguments, "--unsetenv");
  aml_sandbox_add_argument (arguments, name);
}

static void
add_runtime_directories (GPtrArray *arguments)
{
  const char *home = g_get_home_dir ();
  const char *runtime = g_get_user_runtime_dir ();
  g_autofree char *home_local = g_build_filename (home, ".local", NULL);
  g_autofree char *home_share = g_build_filename (home_local, "share", NULL);
  g_autofree char *home_data = g_build_filename (home_share, "AML", NULL);
  g_autofree char *runtime_parent = runtime != NULL
    ? g_path_get_dirname (runtime) : NULL;

  /* Bind destinations live in a new root. Create only the empty directory
   * skeleton needed by the selected game/cache mounts. */
  aml_sandbox_add_directory (arguments, "/home");
  aml_sandbox_add_directory (arguments, home);
  aml_sandbox_add_directory (arguments, home_local);
  aml_sandbox_add_directory (arguments, home_share);
  aml_sandbox_add_directory (arguments, home_data);
  aml_sandbox_add_directory (arguments, "/run");
  aml_sandbox_add_directory (arguments, "/run/user");
  if (runtime_parent != NULL)
    aml_sandbox_add_directory (arguments, runtime_parent);
  if (runtime != NULL)
    aml_sandbox_add_directory (arguments, runtime);
  aml_sandbox_add_directory (arguments, "/tmp/.X11-unix");
}

GPtrArray *
aml_sandbox_wrap_arguments (GPtrArray               *command,
                            const AmlResolvedVersion *resolved,
                            const char               *java_executable,
                            const char               *game_directory,
                            gboolean                  network_enabled,
                            GError                  **error)
{
  g_autoptr (GPtrArray) arguments = NULL;
  g_autofree char *bwrap = NULL;
  g_autofree char *self_executable = NULL;
  const char *broker = NULL;
  g_autoptr (GError) broker_error = NULL;

  g_return_val_if_fail (command != NULL && command->len > 0, NULL);
  g_return_val_if_fail (resolved != NULL, NULL);
  g_return_val_if_fail (game_directory != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  bwrap = g_find_program_in_path ("bwrap");
  if (bwrap == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "bubblewrap (bwrap) is not installed");
      return NULL;
    }

  arguments = g_ptr_array_new_with_free_func (g_free);
  aml_sandbox_add_argument (arguments, bwrap);
  aml_sandbox_add_argument (arguments, "--die-with-parent");
  aml_sandbox_add_argument (arguments, "--new-session");
  aml_sandbox_add_argument (arguments, "--unshare-pid");
  aml_sandbox_add_argument (arguments, "--unshare-uts");
  aml_sandbox_add_argument (arguments, "--unshare-ipc");
  aml_sandbox_add_argument (arguments, "--unshare-cgroup");
  if (!network_enabled)
    aml_sandbox_add_argument (arguments, "--unshare-net");
  add_pair (arguments, "--proc", "/proc");
  add_pair (arguments, "--dev", "/dev");
  add_pair (arguments, "--tmpfs", "/tmp");
  add_runtime_directories (arguments);

  aml_sandbox_add_system_mounts (arguments);
  aml_sandbox_add_game_mounts (arguments, resolved, java_executable,
                               game_directory);
  aml_sandbox_add_display_mounts (arguments);
  broker = aml_open_broker_start (game_directory, &broker_error);
  if (broker == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&broker_error));
      return NULL;
    }

  self_executable = g_file_read_link ("/proc/self/exe", NULL);
  if (self_executable != NULL)
    {
      /* Most mods use xdg-open. Replace only that entry point with AML's
       * one-purpose client; no shell or arbitrary executable is exposed. */
      add_pair (arguments, "--ro-bind", self_executable);
      aml_sandbox_add_argument (arguments, "/usr/bin/xdg-open");
    }
  aml_sandbox_add_argument (arguments, "--bind");
  aml_sandbox_add_argument (arguments, broker);
  aml_sandbox_add_argument (arguments, broker);
  add_environment (arguments, "AML_OPEN_BROKER_SOCKET", broker);
  unset_environment (arguments, "DBUS_SESSION_BUS_ADDRESS");
  unset_environment (arguments, "DBUS_SYSTEM_BUS_ADDRESS");
  add_environment (arguments, "HOME", game_directory);
  add_pair (arguments, "--chdir", game_directory);
  aml_sandbox_add_argument (arguments, "--");

  for (guint i = 0; i < command->len; i++)
    aml_sandbox_add_argument (arguments, g_ptr_array_index (command, i));

  return g_steal_pointer (&arguments);
}
