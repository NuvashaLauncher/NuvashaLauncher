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

void
aml_sandbox_add_display_mounts (GPtrArray *arguments)
{
  const char *runtime = g_get_user_runtime_dir ();
  const char *authority = g_getenv ("XAUTHORITY");

  /* Keep both backends available: GLFW uses Wayland in a Wayland session and
   * can then reach XWayland through DISPLAY when a client needs X11. */
  bind_readonly_if_present (arguments, runtime);
  bind_readonly_if_present (arguments, "/tmp/.X11-unix");
  bind_readonly_if_present (arguments, authority);
}
