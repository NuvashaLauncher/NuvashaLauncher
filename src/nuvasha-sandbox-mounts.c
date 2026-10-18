/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-sandbox-mounts.h"
#include "nuvasha-sandbox-private.h"

#include <glib/gstdio.h>
#include <string.h>

static void
bind_device_if_present (GPtrArray *arguments, const char *path)
{
  if (path != NULL && *path != '\0' && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      nuvasha_sandbox_add_argument (arguments, "--dev-bind-try");
      nuvasha_sandbox_add_argument (arguments, path);
      nuvasha_sandbox_add_argument (arguments, path);
    }
}

static void
bind_writable_if_present (GPtrArray *arguments, const char *path)
{
  if (path != NULL && *path != '\0' && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      nuvasha_sandbox_add_argument (arguments, "--bind-try");
      nuvasha_sandbox_add_argument (arguments, path);
      nuvasha_sandbox_add_argument (arguments, path);
    }
}

static void
bind_parent_readonly (GPtrArray *arguments, const char *path)
{
  g_autofree char *parent = NULL;

  if (path == NULL || *path == '\0')
    return;

  parent = g_path_get_dirname (path);
  nuvasha_sandbox_bind_readonly_if_present (arguments, parent);
}

void
nuvasha_sandbox_add_system_mounts (GPtrArray *arguments)
{
  static const char *const directories[] = {
    "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/sys"
  };

  for (guint i = 0; i < G_N_ELEMENTS (directories); i++)
    nuvasha_sandbox_bind_readonly_if_present (arguments, directories[i]);

  bind_device_if_present (arguments, "/dev/dri");
  bind_device_if_present (arguments, "/dev/nvidiactl");
  bind_device_if_present (arguments, "/dev/nvidia-uvm");
  bind_device_if_present (arguments, "/dev/nvidia-modeset");
  bind_device_if_present (arguments, "/dev/nvidia0");
  nuvasha_sandbox_bind_readonly_if_present (arguments, "/run/udev");
}

void
nuvasha_sandbox_add_game_mounts (GPtrArray *arguments,
                              const NuvashaResolvedVersion *resolved,
                              const char *java_executable,
                              const char *game_directory)
{
  g_autofree char *java_dir = g_path_get_dirname (java_executable);

  nuvasha_sandbox_bind_readonly_if_present (arguments, java_dir);
  nuvasha_sandbox_bind_readonly_if_present (arguments, resolved->assets_directory);
  nuvasha_sandbox_bind_readonly_if_present (arguments, resolved->library_directory);
  bind_parent_readonly (arguments, resolved->client_jar);
  bind_writable_if_present (arguments, game_directory);
  bind_writable_if_present (arguments, resolved->natives_directory);

  /* The logging JVM argument contains the absolute path of a file resolved
   * alongside the other version resources. Only its containing directory is
   * exposed, never the rest of nuvasha's data tree. */
  if (resolved->logging_argument != NULL)
    {
      const char *equals = strchr (resolved->logging_argument, '=');

      if (equals != NULL && equals[1] == '/')
        bind_parent_readonly (arguments, equals + 1);
    }
}
