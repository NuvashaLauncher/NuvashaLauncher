/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-sandbox-display.h"

#include <glib/gstdio.h>

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
add_directory (GPtrArray *arguments, const char *path)
{
  add (arguments, "--dir");
  add (arguments, path);
}

static void
bind_runtime_socket (GPtrArray *arguments, const char *runtime,
                     const char *name)
{
  g_autofree char *path = NULL;

  if (runtime == NULL || name == NULL || *name == '\0')
    return;

  path = g_build_filename (runtime, name, NULL);
  bind_readonly_if_present (arguments, path);
}

void
aml_sandbox_add_display_mounts (GPtrArray *arguments)
{
  const char *runtime = g_get_user_runtime_dir ();
  const char *wayland = g_getenv ("WAYLAND_DISPLAY");
  const char *authority = g_getenv ("XAUTHORITY");

  /* Keep both backends available: GLFW uses Wayland in a Wayland session and
   * can then reach XWayland through DISPLAY when a client needs X11. */
  if (wayland != NULL && !g_path_is_absolute (wayland))
    bind_runtime_socket (arguments, runtime, wayland);

  bind_runtime_socket (arguments, runtime, "pipewire-0");

  /* Some PipeWire setups expose the PulseAudio compatibility socket instead
   * of using pipewire-0 directly. Mount only that socket, not the runtime
   * directory that also contains D-Bus and credential-bearing sockets. */
  if (runtime != NULL)
    {
      g_autofree char *pulse = g_build_filename (runtime, "pulse", NULL);
      g_autofree char *native = g_build_filename (pulse, "native", NULL);

      if (g_file_test (pulse, G_FILE_TEST_IS_DIR))
        add_directory (arguments, pulse);
      bind_readonly_if_present (arguments, native);
    }

  bind_readonly_if_present (arguments, "/tmp/.X11-unix");
  bind_readonly_if_present (arguments, authority);
}
