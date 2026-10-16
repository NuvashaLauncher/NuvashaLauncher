/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-sandbox-mounts.h"

#include <glib/gstdio.h>
#include <string.h>

static void
add (GPtrArray *arguments, const char *value)
{
  g_ptr_array_add (arguments, g_strdup (value));
}

static void
bind_readonly_if_present (GPtrArray *arguments, const char *path)
{
  if (path != NULL && *path != '\0' && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      add (arguments, "--ro-bind-try");
      add (arguments, path);
      add (arguments, path);
    }
}

static void
bind_device_if_present (GPtrArray *arguments, const char *path)
{
  if (path != NULL && *path != '\0' && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      add (arguments, "--dev-bind-try");
      add (arguments, path);
      add (arguments, path);
    }
}

static void
bind_writable_if_present (GPtrArray *arguments, const char *path)
{
  if (path != NULL && *path != '\0' && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      add (arguments, "--bind-try");
      add (arguments, path);
      add (arguments, path);
    }
}

static void
bind_parent_readonly (GPtrArray *arguments, const char *path)
{
  g_autofree char *parent = NULL;

  if (path == NULL || *path == '\0')
    return;

  parent = g_path_get_dirname (path);
  bind_readonly_if_present (arguments, parent);
}

void
aml_sandbox_add_system_mounts (GPtrArray *arguments)
{
  static const char *const directories[] = {
    "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc"
  };

  for (guint i = 0; i < G_N_ELEMENTS (directories); i++)
    bind_readonly_if_present (arguments, directories[i]);

  bind_device_if_present (arguments, "/dev/dri");
  bind_device_if_present (arguments, "/dev/nvidiactl");
  bind_device_if_present (arguments, "/dev/nvidia-uvm");
  bind_device_if_present (arguments, "/dev/nvidia-modeset");
  bind_device_if_present (arguments, "/dev/nvidia0");
}

void
aml_sandbox_add_game_mounts (GPtrArray *arguments,
                              const AmlResolvedVersion *resolved,
                              const char *java_executable,
                              const char *game_directory)
{
  g_autofree char *java_dir = g_path_get_dirname (java_executable);

  bind_readonly_if_present (arguments, java_dir);
  bind_readonly_if_present (arguments, resolved->assets_directory);
  bind_readonly_if_present (arguments, resolved->library_directory);
  bind_parent_readonly (arguments, resolved->client_jar);
  bind_writable_if_present (arguments, resolved->natives_directory);
  bind_writable_if_present (arguments, game_directory);

  /* The logging JVM argument contains the absolute path of a file resolved
   * alongside the other version resources. Only its containing directory is
   * exposed, never the rest of AML's data tree. */
  if (resolved->logging_argument != NULL)
    {
      const char *equals = strchr (resolved->logging_argument, '=');

      if (equals != NULL && equals[1] == '/')
        bind_parent_readonly (arguments, equals + 1);
    }
}
