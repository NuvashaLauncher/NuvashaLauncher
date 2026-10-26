/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherVersionRow: Minecraft version picker used during creation and
 * settings editing. It loads versions from Mojang's manifest and exposes
 * loading and retry states. */

#ifndef nuvashalauncher_VERSION_ROW_H
#define nuvashalauncher_VERSION_ROW_H

#include <adwaita.h>

#include "nuvashalauncher-settings.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_VERSION_ROW (nuvashalauncher_version_row_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherVersionRow, nuvashalauncher_version_row, nuvashalauncher, VERSION_ROW,
                      AdwComboRow)

/* A row with an empty list, waiting for a load(). */
nuvashalauncherVersionRow *nuvashalauncher_version_row_new (void);

/* Fetches the manifest and fills the row with the versions it lists,
 * releases only unless @settings says to show snapshots and the old
 * versions too.
 *
 * @selected, when given, is the version the row has to show as picked
 * once the list is there; it stays in the list even when the filter
 * would hide its type. While the load runs the row is insensitive and
 * answers nothing, and a failure replaces the list with a message and
 * a Retry button.
 *
 * Loading again cancels the load that is still running. */
void nuvashalauncher_version_row_load (nuvashalauncherVersionRow *self,
                           nuvashalauncherSettings   *settings,
                           const char    *selected);

/* The version picked, or NULL while the row has no list, is loading,
 * failed, or has nothing picked. The row emits notify::selected when
 * that changes. */
const char *nuvashalauncher_version_row_get_selected (nuvashalauncherVersionRow *self);

/* Picks @version, or leaves the row alone when it is not in the list
 * or there is no list to pick from. Used to put a row back where it
 * was after a change that could not be saved. */
void nuvashalauncher_version_row_select (nuvashalauncherVersionRow *self, const char *version);

G_END_DECLS

#endif /* nuvashalauncher_VERSION_ROW_H */
