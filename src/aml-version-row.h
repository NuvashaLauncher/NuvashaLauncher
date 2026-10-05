/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlVersionRow: the "Minecraft Version" row, used both when an
 * instance is created and when its settings are edited.
 *
 * The row is a list of whatever Mojang's version manifest holds, so it
 * knows no version of its own: it shows that it is fetching the list,
 * that fetching failed and how to try again, and which version is
 * picked once the list is there. */

#ifndef AML_VERSION_ROW_H
#define AML_VERSION_ROW_H

#include <adwaita.h>

#include "aml-settings.h"

G_BEGIN_DECLS

#define AML_TYPE_VERSION_ROW (aml_version_row_get_type ())

G_DECLARE_FINAL_TYPE (AmlVersionRow, aml_version_row, AML, VERSION_ROW,
                      AdwComboRow)

/* A row with an empty list, waiting for a load(). */
AmlVersionRow *aml_version_row_new (void);

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
void aml_version_row_load (AmlVersionRow *self,
                           AmlSettings   *settings,
                           const char    *selected);

/* The version picked, or NULL while the row has no list, is loading,
 * failed, or has nothing picked. The row emits notify::selected when
 * that changes. */
const char *aml_version_row_get_selected (AmlVersionRow *self);

/* Picks @version, or leaves the row alone when it is not in the list
 * or there is no list to pick from. Used to put a row back where it
 * was after a change that could not be saved. */
void aml_version_row_select (AmlVersionRow *self, const char *version);

G_END_DECLS

#endif /* AML_VERSION_ROW_H */
