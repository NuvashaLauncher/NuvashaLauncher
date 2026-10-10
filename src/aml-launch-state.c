/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-launcher.h"

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* One table for the enum type of the signal parameter and the names the
 * enum members are spelled with. */
static const GEnumValue launch_state_values[] = {
  { AML_LAUNCH_PREPARING,      "AML_LAUNCH_PREPARING",      "Preparing"     },
  { AML_LAUNCH_DOWNLOADING,    "AML_LAUNCH_DOWNLOADING",    "Downloading"   },
  { AML_LAUNCH_LAUNCHING,      "AML_LAUNCH_LAUNCHING",      "Launching"     },
  { AML_LAUNCH_RUNNING,        "AML_LAUNCH_RUNNING",        "Running"       },
  { AML_LAUNCH_STOPPING,       "AML_LAUNCH_STOPPING",       "Stopping"      },
  { AML_LAUNCH_STOPPED,        "AML_LAUNCH_STOPPED",        "Stopped"       },
  { AML_LAUNCH_CRASHED,        "AML_LAUNCH_CRASHED",        "Crashed"       },
  { AML_LAUNCH_FAILED,         "AML_LAUNCH_FAILED",         "Failed"        },
  { AML_LAUNCH_NEEDS_SIGN_IN,  "AML_LAUNCH_NEEDS_SIGN_IN",  "Needs sign-in" },
  { 0, NULL, NULL }
};

GType
aml_launch_state_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType state_type = g_enum_register_static ("AmlLaunchState",
                                                 launch_state_values);
      g_once_init_leave (&type_id, state_type);
    }

  return type_id;
}

const char *
aml_launch_state_to_string (AmlLaunchState state)
{
  for (guint i = 0; launch_state_values[i].value_name != NULL; i++)
    {
      if ((AmlLaunchState) launch_state_values[i].value == state)
        return launch_state_values[i].value_nick;
    }

  return "Unknown";
}

gboolean
aml_launch_state_is_busy (AmlLaunchState state)
{
  return state == AML_LAUNCH_PREPARING || state == AML_LAUNCH_DOWNLOADING ||
         state == AML_LAUNCH_LAUNCHING || state == AML_LAUNCH_RUNNING ||
         state == AML_LAUNCH_STOPPING;
}

