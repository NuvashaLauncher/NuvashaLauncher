/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlLoaderRow: the two rows that choose the mod loader of an instance
 * and one of its versions.
 *
 * The first row is the loaders AML installs, and nothing else: a loader
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

#ifndef AML_LOADER_ROW_H
#define AML_LOADER_ROW_H

#include <adwaita.h>

#include "aml-loaders.h"
#include "aml-settings.h"

G_BEGIN_DECLS

#define AML_TYPE_LOADER_ROW (aml_loader_row_get_type ())

G_DECLARE_FINAL_TYPE (AmlLoaderRow, aml_loader_row, AML, LOADER_ROW, GtkBox)

/* "changed" is what a screen stores the choice with: ask get_loader()
 * and get_version() and write them down. */
enum {
  AML_LOADER_ROW_SIGNAL_CHANGED,
  AML_LOADER_ROW_N_SIGNALS
};

/* Picks @loader, and @loader_version of it for @game_version. Nothing is
 * loaded when @loader is vanilla, which has no versions. */
AmlLoaderRow *aml_loader_row_new (void);

/* Loads the versions of @loader for @game_version, picking
   @loader_version, and remembers @game_version for the loads that come
   after it. This is what a screen calls when the Minecraft version
   changes. */
void aml_loader_row_load (AmlLoaderRow *self,
                          AmlSettings   *settings,
                          AmlLoader      loader,
                          const char    *loader_version,
                          const char    *game_version);

/* The loader that is picked. Vanilla until something else is. */
AmlLoader aml_loader_row_get_loader (AmlLoaderRow *self);

/* The version that is picked, or NULL when there is none to pick: while
 * loading, when the loader has nothing for that Minecraft version and
 * when the list could not be fetched. Vanilla has no version at all. */
const char *aml_loader_row_get_version (AmlLoaderRow *self);

G_END_DECLS

#endif /* AML_LOADER_ROW_H */