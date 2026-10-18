/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-launcher.h"

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* One table for the enum type of the signal parameter and the names the
 * enum members are spelled with. */
static const GEnumValue launch_state_values[] = {
  { comfy_LAUNCH_PREPARING,      "comfy_LAUNCH_PREPARING",      "Preparing"     },
  { comfy_LAUNCH_DOWNLOADING,    "comfy_LAUNCH_DOWNLOADING",    "Downloading"   },
  { comfy_LAUNCH_LAUNCHING,      "comfy_LAUNCH_LAUNCHING",      "Launching"     },
  { comfy_LAUNCH_RUNNING,        "comfy_LAUNCH_RUNNING",        "Running"       },
  { comfy_LAUNCH_STOPPING,       "comfy_LAUNCH_STOPPING",       "Stopping"      },
  { comfy_LAUNCH_STOPPED,        "comfy_LAUNCH_STOPPED",        "Stopped"       },
  { comfy_LAUNCH_CRASHED,        "comfy_LAUNCH_CRASHED",        "Crashed"       },
  { comfy_LAUNCH_FAILED,         "comfy_LAUNCH_FAILED",         "Failed"        },
  { comfy_LAUNCH_NEEDS_SIGN_IN,  "comfy_LAUNCH_NEEDS_SIGN_IN",  "Needs sign-in" },
  { 0, NULL, NULL }
};

GType
comfy_launch_state_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType state_type = g_enum_register_static ("ComfyLaunchState",
                                                 launch_state_values);
      g_once_init_leave (&type_id, state_type);
    }

  return type_id;
}

const char *
comfy_launch_state_to_string (ComfyLaunchState state)
{
  for (guint i = 0; launch_state_values[i].value_name != NULL; i++)
    {
      if ((ComfyLaunchState) launch_state_values[i].value == state)
        return launch_state_values[i].value_nick;
    }

  return "Unknown";
}

gboolean
comfy_launch_state_is_busy (ComfyLaunchState state)
{
  return state == comfy_LAUNCH_PREPARING || state == comfy_LAUNCH_DOWNLOADING ||
         state == comfy_LAUNCH_LAUNCHING || state == comfy_LAUNCH_RUNNING ||
         state == comfy_LAUNCH_STOPPING;
}

