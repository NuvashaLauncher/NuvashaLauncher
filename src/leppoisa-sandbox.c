/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-sandbox.h"
#include "leppoisa-sandbox-private.h"
#include "leppoisa-sandbox-display.h"
#include "leppoisa-sandbox-mounts.h"
#include "leppoisa-open-broker.h"

#include <unistd.h>


void
leppoisa_sandbox_add_argument (GPtrArray *arguments, const char *value)
{
  g_ptr_array_add (arguments, g_strdup (value));
}

void
leppoisa_sandbox_add_directory (GPtrArray *arguments, const char *path)
{
  leppoisa_sandbox_add_argument (arguments, "--dir");
  leppoisa_sandbox_add_argument (arguments, path);
}

void
leppoisa_sandbox_bind_readonly_if_present (GPtrArray *arguments, const char *path)
{
  if (path != NULL && *path != '\0' && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      leppoisa_sandbox_add_argument (arguments, "--ro-bind-try");
      leppoisa_sandbox_add_argument (arguments, path);
      leppoisa_sandbox_add_argument (arguments, path);
    }
}

static void
add_pair (GPtrArray *arguments, const char *option, const char *value)
{
  leppoisa_sandbox_add_argument (arguments, option);
  leppoisa_sandbox_add_argument (arguments, value);
}

static void
add_environment (GPtrArray *arguments, const char *name, const char *value)
{
  leppoisa_sandbox_add_argument (arguments, "--setenv");
  leppoisa_sandbox_add_argument (arguments, name);
  leppoisa_sandbox_add_argument (arguments, value);
}

static void
unset_environment (GPtrArray *arguments, const char *name)
{
  leppoisa_sandbox_add_argument (arguments, "--unsetenv");
  leppoisa_sandbox_add_argument (arguments, name);
}

static void
add_runtime_directories (GPtrArray *arguments)
{
  const char *home = g_get_home_dir ();
  const char *runtime = g_get_user_runtime_dir ();
  g_autofree char *home_local = g_build_filename (home, ".local", NULL);
  g_autofree char *home_share = g_build_filename (home_local, "share", NULL);
  g_autofree char *home_data = g_build_filename (home_share, "leppoisa", NULL);
  g_autofree char *runtime_parent = runtime != NULL
    ? g_path_get_dirname (runtime) : NULL;

  /* Bind destinations live in a new root. Create only the empty directory
   * skeleton needed by the selected game/cache mounts. */
  leppoisa_sandbox_add_directory (arguments, "/home");
  leppoisa_sandbox_add_directory (arguments, home);
  leppoisa_sandbox_add_directory (arguments, home_local);
  leppoisa_sandbox_add_directory (arguments, home_share);
  leppoisa_sandbox_add_directory (arguments, home_data);
  leppoisa_sandbox_add_directory (arguments, "/run");
  leppoisa_sandbox_add_directory (arguments, "/run/user");
  if (runtime_parent != NULL)
    leppoisa_sandbox_add_directory (arguments, runtime_parent);
  if (runtime != NULL)
    leppoisa_sandbox_add_directory (arguments, runtime);
  leppoisa_sandbox_add_directory (arguments, "/tmp/.X11-unix");
}

GPtrArray *
leppoisa_sandbox_wrap_arguments (GPtrArray               *command,
                            const LeppoisaResolvedVersion *resolved,
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
  leppoisa_sandbox_add_argument (arguments, bwrap);
  leppoisa_sandbox_add_argument (arguments, "--die-with-parent");
  leppoisa_sandbox_add_argument (arguments, "--new-session");
  leppoisa_sandbox_add_argument (arguments, "--unshare-pid");
  leppoisa_sandbox_add_argument (arguments, "--unshare-uts");
  leppoisa_sandbox_add_argument (arguments, "--unshare-ipc");
  leppoisa_sandbox_add_argument (arguments, "--unshare-cgroup");
  if (!network_enabled)
    leppoisa_sandbox_add_argument (arguments, "--unshare-net");
  add_pair (arguments, "--proc", "/proc");
  add_pair (arguments, "--dev", "/dev");
  add_pair (arguments, "--tmpfs", "/tmp");
  add_runtime_directories (arguments);

  leppoisa_sandbox_add_system_mounts (arguments);
  leppoisa_sandbox_add_game_mounts (arguments, resolved, java_executable,
                               game_directory);
  leppoisa_sandbox_add_display_mounts (arguments);
  broker = leppoisa_open_broker_start (game_directory, &broker_error);
  if (broker == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&broker_error));
      return NULL;
    }

  self_executable = g_file_read_link ("/proc/self/exe", NULL);
  if (self_executable != NULL)
    {
      /* Most mods use xdg-open. Replace only that entry point with leppoisa's
       * one-purpose client; no shell or arbitrary executable is exposed. */
      add_pair (arguments, "--ro-bind", self_executable);
      leppoisa_sandbox_add_argument (arguments, "/usr/bin/xdg-open");
    }
  leppoisa_sandbox_add_argument (arguments, "--bind");
  leppoisa_sandbox_add_argument (arguments, broker);
  leppoisa_sandbox_add_argument (arguments, broker);
  add_environment (arguments, "leppoisa_OPEN_BROKER_SOCKET", broker);
  unset_environment (arguments, "DBUS_SESSION_BUS_ADDRESS");
  unset_environment (arguments, "DBUS_SYSTEM_BUS_ADDRESS");
  add_environment (arguments, "HOME", game_directory);
  add_pair (arguments, "--chdir", game_directory);
  leppoisa_sandbox_add_argument (arguments, "--");

  for (guint i = 0; i < command->len; i++)
    leppoisa_sandbox_add_argument (arguments, g_ptr_array_index (command, i));

  return g_steal_pointer (&arguments);
}
