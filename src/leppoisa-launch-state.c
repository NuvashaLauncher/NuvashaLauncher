/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-launcher.h"

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* One table for the enum type of the signal parameter and the names the
 * enum members are spelled with. */
static const GEnumValue launch_state_values[] = {
  { leppoisa_LAUNCH_PREPARING,      "leppoisa_LAUNCH_PREPARING",      "Preparing"     },
  { leppoisa_LAUNCH_DOWNLOADING,    "leppoisa_LAUNCH_DOWNLOADING",    "Downloading"   },
  { leppoisa_LAUNCH_LAUNCHING,      "leppoisa_LAUNCH_LAUNCHING",      "Launching"     },
  { leppoisa_LAUNCH_RUNNING,        "leppoisa_LAUNCH_RUNNING",        "Running"       },
  { leppoisa_LAUNCH_STOPPING,       "leppoisa_LAUNCH_STOPPING",       "Stopping"      },
  { leppoisa_LAUNCH_STOPPED,        "leppoisa_LAUNCH_STOPPED",        "Stopped"       },
  { leppoisa_LAUNCH_CRASHED,        "leppoisa_LAUNCH_CRASHED",        "Crashed"       },
  { leppoisa_LAUNCH_FAILED,         "leppoisa_LAUNCH_FAILED",         "Failed"        },
  { leppoisa_LAUNCH_NEEDS_SIGN_IN,  "leppoisa_LAUNCH_NEEDS_SIGN_IN",  "Needs sign-in" },
  { 0, NULL, NULL }
};

GType
leppoisa_launch_state_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType state_type = g_enum_register_static ("LeppoisaLaunchState",
                                                 launch_state_values);
      g_once_init_leave (&type_id, state_type);
    }

  return type_id;
}

const char *
leppoisa_launch_state_to_string (LeppoisaLaunchState state)
{
  for (guint i = 0; launch_state_values[i].value_name != NULL; i++)
    {
      if ((LeppoisaLaunchState) launch_state_values[i].value == state)
        return launch_state_values[i].value_nick;
    }

  return "Unknown";
}

gboolean
leppoisa_launch_state_is_busy (LeppoisaLaunchState state)
{
  return state == leppoisa_LAUNCH_PREPARING || state == leppoisa_LAUNCH_DOWNLOADING ||
         state == leppoisa_LAUNCH_LAUNCHING || state == leppoisa_LAUNCH_RUNNING ||
         state == leppoisa_LAUNCH_STOPPING;
}

