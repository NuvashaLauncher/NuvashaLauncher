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
  { nuvashalauncher_LAUNCH_PREPARING,      "nuvashalauncher_LAUNCH_PREPARING",      "Preparing"     },
  { nuvashalauncher_LAUNCH_DOWNLOADING,    "nuvashalauncher_LAUNCH_DOWNLOADING",    "Downloading"   },
  { nuvashalauncher_LAUNCH_LAUNCHING,      "nuvashalauncher_LAUNCH_LAUNCHING",      "Launching"     },
  { nuvashalauncher_LAUNCH_RUNNING,        "nuvashalauncher_LAUNCH_RUNNING",        "Running"       },
  { nuvashalauncher_LAUNCH_STOPPING,       "nuvashalauncher_LAUNCH_STOPPING",       "Stopping"      },
  { nuvashalauncher_LAUNCH_STOPPED,        "nuvashalauncher_LAUNCH_STOPPED",        "Stopped"       },
  { nuvashalauncher_LAUNCH_CRASHED,        "nuvashalauncher_LAUNCH_CRASHED",        "Crashed"       },
  { nuvashalauncher_LAUNCH_FAILED,         "nuvashalauncher_LAUNCH_FAILED",         "Failed"        },
  { nuvashalauncher_LAUNCH_NEEDS_SIGN_IN,  "nuvashalauncher_LAUNCH_NEEDS_SIGN_IN",  "Needs sign-in" },
  { 0, NULL, NULL }
};

GType
nuvashalauncher_launch_state_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType state_type = g_enum_register_static ("nuvashalauncherLaunchState",
                                                 launch_state_values);
      g_once_init_leave (&type_id, state_type);
    }

  return type_id;
}

const char *
nuvashalauncher_launch_state_to_string (nuvashalauncherLaunchState state)
{
  for (guint i = 0; launch_state_values[i].value_name != NULL; i++)
    {
      if ((nuvashalauncherLaunchState) launch_state_values[i].value == state)
        return launch_state_values[i].value_nick;
    }

  return "Unknown";
}

gboolean
nuvashalauncher_launch_state_is_busy (nuvashalauncherLaunchState state)
{
  return state == nuvashalauncher_LAUNCH_PREPARING || state == nuvashalauncher_LAUNCH_DOWNLOADING ||
         state == nuvashalauncher_LAUNCH_LAUNCHING || state == nuvashalauncher_LAUNCH_RUNNING ||
         state == nuvashalauncher_LAUNCH_STOPPING;
}
