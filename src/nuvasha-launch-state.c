/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-launcher.h"

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* One table for the enum type of the signal parameter and the names the
 * enum members are spelled with. */
static const GEnumValue launch_state_values[] = {
  { nuvasha_LAUNCH_PREPARING,      "nuvasha_LAUNCH_PREPARING",      "Preparing"     },
  { nuvasha_LAUNCH_DOWNLOADING,    "nuvasha_LAUNCH_DOWNLOADING",    "Downloading"   },
  { nuvasha_LAUNCH_LAUNCHING,      "nuvasha_LAUNCH_LAUNCHING",      "Launching"     },
  { nuvasha_LAUNCH_RUNNING,        "nuvasha_LAUNCH_RUNNING",        "Running"       },
  { nuvasha_LAUNCH_STOPPING,       "nuvasha_LAUNCH_STOPPING",       "Stopping"      },
  { nuvasha_LAUNCH_STOPPED,        "nuvasha_LAUNCH_STOPPED",        "Stopped"       },
  { nuvasha_LAUNCH_CRASHED,        "nuvasha_LAUNCH_CRASHED",        "Crashed"       },
  { nuvasha_LAUNCH_FAILED,         "nuvasha_LAUNCH_FAILED",         "Failed"        },
  { nuvasha_LAUNCH_NEEDS_SIGN_IN,  "nuvasha_LAUNCH_NEEDS_SIGN_IN",  "Needs sign-in" },
  { 0, NULL, NULL }
};

GType
nuvasha_launch_state_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType state_type = g_enum_register_static ("NuvashaLaunchState",
                                                 launch_state_values);
      g_once_init_leave (&type_id, state_type);
    }

  return type_id;
}

const char *
nuvasha_launch_state_to_string (NuvashaLaunchState state)
{
  for (guint i = 0; launch_state_values[i].value_name != NULL; i++)
    {
      if ((NuvashaLaunchState) launch_state_values[i].value == state)
        return launch_state_values[i].value_nick;
    }

  return "Unknown";
}

gboolean
nuvasha_launch_state_is_busy (NuvashaLaunchState state)
{
  return state == nuvasha_LAUNCH_PREPARING || state == nuvasha_LAUNCH_DOWNLOADING ||
         state == nuvasha_LAUNCH_LAUNCHING || state == nuvasha_LAUNCH_RUNNING ||
         state == nuvasha_LAUNCH_STOPPING;
}
