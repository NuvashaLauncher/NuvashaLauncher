/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaLoaderRow: the two rows that choose the mod loader of an instance
 * and one of its versions.
 *
 * The first row is the loaders leppoisa installs, and nothing else: a loader
 * that has not been seen to start a game here is not in the list. The
 * second is the versions of that loader for one Minecraft version,
 * fetched when either of the two above changes: it says that it is
 * loading, it says why a list could not be fetched and offers a Retry,
 * and it says when the loader has nothing for that Minecraft version.
 *
 * A loader the user does not call stable is written with "(unstable)"
 * after it, and stays out of the list entirely unless the settings ask
 * for it. The version an instance already uses stays in the list even
 * when it would be hidden, so that opening an instance never drops the
 * loader it runs.
 *
 * The row answers nothing at all while it is loading, empty or failed:
 * a screen that saves what it is told then saves nothing. */

#ifndef leppoisa_LOADER_ROW_H
#define leppoisa_LOADER_ROW_H

#include <adwaita.h>

#include "leppoisa-loaders.h"
#include "leppoisa-settings.h"

G_BEGIN_DECLS

#define leppoisa_TYPE_LOADER_ROW (leppoisa_loader_row_get_type ())

/* Not a widget: the two rows are plain AdwComboRows that a screen adds
 * to the AdwPreferencesGroup it already has, so they are drawn and drop
 * their popups the same way as every other dropdown on that page. An
 * AdwComboRow that is not a row of a group has no list box to press
 * into, and pressing it does nothing at all. */
G_DECLARE_FINAL_TYPE (LeppoisaLoaderRow, leppoisa_loader_row, leppoisa, LOADER_ROW, GObject)

/* "changed" is what a screen stores the choice with: ask get_loader()
 * and get_version() and write them down. It fires when the user picks a
 * version and once a load settles on one, so a screen is told about a
 * preselected version too. Changing the loader reloads the version row
 * here, with no help from the screen. */
enum {
  leppoisa_LOADER_ROW_SIGNAL_CHANGED,
  leppoisa_LOADER_ROW_N_SIGNALS
};

/* Makes the two rows, holding vanilla and no version until loaded. */
LeppoisaLoaderRow *leppoisa_loader_row_new (void);

/* The "Loader" row. Add it to the group of the page, next to the
 * dropdowns that are already there. */
GtkWidget *leppoisa_loader_row_get_loader_row (LeppoisaLoaderRow *self);

/* The "Loader Version" row, for the same group. */
GtkWidget *leppoisa_loader_row_get_version_row (LeppoisaLoaderRow *self);

/* Loads the versions of @loader for @game_version, picking
   @loader_version, and remembers @game_version for the loads that come
   after it. This is what a screen calls when the Minecraft version
   changes. */
void leppoisa_loader_row_load (LeppoisaLoaderRow *self,
                          LeppoisaSettings   *settings,
                          LeppoisaLoader      loader,
                          const char    *loader_version,
                          const char    *game_version);

/* The loader that is picked. Vanilla until something else is. */
LeppoisaLoader leppoisa_loader_row_get_loader (LeppoisaLoaderRow *self);

/* The version that is picked, or NULL when there is none to pick: while
 * loading, when the loader has nothing for that Minecraft version and
 * when the list could not be fetched. Vanilla has no version at all. */
const char *leppoisa_loader_row_get_version (LeppoisaLoaderRow *self);

/* Locks or unlocks both rows. While the game of the instance runs, a
 * loader cannot be changed under it, so a screen locks the rows and
 * unlocks them again when the game stops. */
void leppoisa_loader_row_set_blocked (LeppoisaLoaderRow *self, gboolean blocked);

G_END_DECLS

#endif /* leppoisa_LOADER_ROW_H */