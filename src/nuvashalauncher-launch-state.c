/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-launcher.h"

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* One table for the enum type of the signal parameter and the names the
 * enum members are spelled with. */
static const GEnumValue launch_state_values[] = {
  { NUVASHALAUNCHER_LAUNCH_PREPARING,      "NUVASHALAUNCHER_LAUNCH_PREPARING",      "Preparing"     },
  { NUVASHALAUNCHER_LAUNCH_DOWNLOADING,    "NUVASHALAUNCHER_LAUNCH_DOWNLOADING",    "Downloading"   },
  { NUVASHALAUNCHER_LAUNCH_LAUNCHING,      "NUVASHALAUNCHER_LAUNCH_LAUNCHING",      "Launching"     },
  { NUVASHALAUNCHER_LAUNCH_RUNNING,        "NUVASHALAUNCHER_LAUNCH_RUNNING",        "Running"       },
  { NUVASHALAUNCHER_LAUNCH_STOPPING,       "NUVASHALAUNCHER_LAUNCH_STOPPING",       "Stopping"      },
  { NUVASHALAUNCHER_LAUNCH_STOPPED,        "NUVASHALAUNCHER_LAUNCH_STOPPED",        "Stopped"       },
  { NUVASHALAUNCHER_LAUNCH_CRASHED,        "NUVASHALAUNCHER_LAUNCH_CRASHED",        "Crashed"       },
  { NUVASHALAUNCHER_LAUNCH_FAILED,         "NUVASHALAUNCHER_LAUNCH_FAILED",         "Failed"        },
  { NUVASHALAUNCHER_LAUNCH_NEEDS_SIGN_IN,  "NUVASHALAUNCHER_LAUNCH_NEEDS_SIGN_IN",  "Needs sign-in" },
  { 0, NULL, NULL }
};

GType
nuvashalauncher_launch_state_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType state_type = g_enum_register_static ("NuvashaLauncherLaunchState",
                                                 launch_state_values);
      g_once_init_leave (&type_id, state_type);
    }

  return type_id;
}

const char *
nuvashalauncher_launch_state_to_string (NuvashaLauncherLaunchState state)
{
  for (guint i = 0; launch_state_values[i].value_name != NULL; i++)
    {
      if ((NuvashaLauncherLaunchState) launch_state_values[i].value == state)
        return launch_state_values[i].value_nick;
    }

  return "Unknown";
}

gboolean
nuvashalauncher_launch_state_is_busy (NuvashaLauncherLaunchState state)
{
  return state == NUVASHALAUNCHER_LAUNCH_PREPARING || state == NUVASHALAUNCHER_LAUNCH_DOWNLOADING ||
         state == NUVASHALAUNCHER_LAUNCH_LAUNCHING || state == NUVASHALAUNCHER_LAUNCH_RUNNING ||
         state == NUVASHALAUNCHER_LAUNCH_STOPPING;
}
