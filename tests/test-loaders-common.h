/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncher-loaders.
 *
 * Every loader answers with a document of its own, and what nuvashalauncher has to
 * get right is reading it: which versions a loader has for one Minecraft
 * version, which of those it calls stable, and the document those
 * versions are, whose libraries publish a maven address instead of a
 * download.
 *
 * The answers come from the fixtures in tests/fixtures, trimmed from
 * what the loaders themselves published, so nothing here needs the
 * network. */

#include "nuvashalauncher-loaders.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-minecraft-resolve.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#ifndef nuvashalauncher_TEST_FIXTURES
#define nuvashalauncher_TEST_FIXTURES "tests/fixtures"
#endif

static const nuvashalauncherLoaderVersion *
version_at (GPtrArray *versions, guint index)
{
  g_assert_cmpuint (index, <, versions->len);

  return g_ptr_array_index (versions, index);
}

/* ---------------------------------------------------------- *
 * The loaders that are in the lists
 * ---------------------------------------------------------- */









/* ---------------------------------------------------------- *
 * The loaders that publish an installer
 * ---------------------------------------------------------- */



/* Which versions of a loader run which Minecraft version: both name their
   versions after the game they run, Forge with a dash between the two and
   NeoForge with a dot. */


/* The versions of NeoForge for one Minecraft version, out of everything
   its maven holds. */


/* The same for Forge, whose names carry a dash and whose releases are
   releases even with a dash in the name. */


/* An answer that is not a list of versions is refused, whatever the
   loader. */


/* ---------------------------------------------------------- *
 * The document of a loader version
 * ---------------------------------------------------------- */

static const nuvashalauncherLibrary *
find_library (nuvashalauncherVersionDescription *description, const char *name)
{
  for (guint i = 0; i < description->libraries->len; i++)
    {
      const nuvashalauncherLibrary *library = g_ptr_array_index (description->libraries, i);

      if (g_strcmp0 (library->name, name) == 0)
        return library;
    }

  return NULL;
}



/* Quilt publishes its libraries without a hash at all, so the document
   has to be usable even when nothing can be verified. */




/* ---------------------------------------------------------- *
 * What a version list shows
 * ---------------------------------------------------------- */

/* The display policy: unstable versions are hidden unless the setting
 * asks for them, a version an instance runs is kept whatever its state,
 * and the newest stable one is preselected. */


/* Both entry points refuse what the other one is for, and they say which
 * of them to use instead. These answers arrive through the main loop, as
 * every answer of this module does. */

typedef struct
{
  GMainLoop *loop;
  GError *error;
  nuvashalauncherLoaderInstall *install;
  nuvashalauncherVersionDescription *profile;
  GPtrArray *versions;
  guint watchdog;
  nuvashalauncherTaskScope *operation;
} Wait;

static gboolean
watchdog_fired (gpointer user_data)
{
  Wait *wait = user_data;

  g_main_loop_quit (wait->loop);

  return G_SOURCE_REMOVE;
}

static void
on_install_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  Wait *wait = user_data;

  wait->install = nuvashalauncher_loaders_install_finish (result, &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

static void
on_profile_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  Wait *wait = user_data;

  wait->profile = nuvashalauncher_loaders_profile_finish (result, &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

static void
on_versions_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                      gpointer user_data)
{
  Wait *wait = user_data;

  wait->versions = nuvashalauncher_loaders_versions_finish (result, &wait->error);
  nuvashalauncher_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

/* A load that was cancelled hands no list to the screen that started it,
   which is what lets the screen drop an answer that belongs to a game
   version or a loader it has moved on from. */


/* A loader that publishes a version document of its own has no installer,
   and asking for one says so instead of looking for a jar that is not
   there. */


/* The other way round: a loader that publishes an installer has no
   profile to fetch, and the answer names the install that has it. */
