/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-sandbox.h"
#include "aml-sandbox-display.h"
#include "aml-sandbox-mounts.h"


static void
add (GPtrArray *arguments, const char *value)
{
  g_ptr_array_add (arguments, g_strdup (value));
}

static void
add_pair (GPtrArray *arguments, const char *option, const char *value)
{
  add (arguments, option);
  add (arguments, value);
}

static void
add_environment (GPtrArray *arguments, const char *name, const char *value)
{
  add (arguments, "--setenv");
  add (arguments, name);
  add (arguments, value);
}

static void
add_directory (GPtrArray *arguments, const char *path)
{
  add (arguments, "--dir");
  add (arguments, path);
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
  add_directory (arguments, "/home");
  add_directory (arguments, home);
  add_directory (arguments, home_local);
  add_directory (arguments, home_share);
  add_directory (arguments, home_data);
  add_directory (arguments, "/run");
  add_directory (arguments, "/run/user");
  if (runtime_parent != NULL)
    add_directory (arguments, runtime_parent);
  if (runtime != NULL)
    add_directory (arguments, runtime);
  add_directory (arguments, "/tmp/.X11-unix");
}

GPtrArray *
aml_sandbox_wrap_arguments (GPtrArray               *command,
                            const AmlResolvedVersion *resolved,
                            const char               *java_executable,
                            const char               *game_directory,
                            GError                  **error)
{
  g_autoptr (GPtrArray) arguments = NULL;
  g_autofree char *bwrap = NULL;

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
  add (arguments, bwrap);
  add (arguments, "--die-with-parent");
  add (arguments, "--new-session");
  add (arguments, "--unshare-pid");
  add (arguments, "--unshare-uts");
  add (arguments, "--unshare-ipc");
  add (arguments, "--unshare-cgroup");
  add_pair (arguments, "--proc", "/proc");
  add_pair (arguments, "--dev", "/dev");
  add_pair (arguments, "--tmpfs", "/tmp");
  add_runtime_directories (arguments);

  aml_sandbox_add_system_mounts (arguments);
  aml_sandbox_add_game_mounts (arguments, resolved, java_executable,
                               game_directory);
  aml_sandbox_add_display_mounts (arguments);
  add_environment (arguments, "HOME", game_directory);
  add_pair (arguments, "--chdir", game_directory);
  add (arguments, "--");

  for (guint i = 0; i < command->len; i++)
    add (arguments, g_ptr_array_index (command, i));

  return g_steal_pointer (&arguments);
}
