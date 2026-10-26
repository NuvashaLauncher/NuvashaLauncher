/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <glib.h>

#include "nuvashalauncher-launcher.h"

static void
test_state_names (void)
{
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_PREPARING), ==,
                   "Preparing");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_DOWNLOADING), ==,
                   "Downloading");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_LAUNCHING), ==,
                   "Launching");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_RUNNING), ==,
                   "Running");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_STOPPING), ==,
                   "Stopping");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_STOPPED), ==,
                   "Stopped");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_CRASHED), ==,
                   "Crashed");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_FAILED), ==,
                   "Failed");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (nuvashalauncher_LAUNCH_NEEDS_SIGN_IN), ==,
                   "Needs sign-in");
  g_print ("ok - launch states have stable display names\n");
}

static void
test_busy_states (void)
{
  g_assert_true (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_PREPARING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_DOWNLOADING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_LAUNCHING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_RUNNING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_STOPPING));
  g_assert_false (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_STOPPED));
  g_assert_false (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_CRASHED));
  g_assert_false (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_FAILED));
  g_assert_false (nuvashalauncher_launch_state_is_busy (nuvashalauncher_LAUNCH_NEEDS_SIGN_IN));
  g_print ("ok - only active launch states are busy\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Launch state\n");
  test_state_names ();
  test_busy_states ();
  return 0;
}
