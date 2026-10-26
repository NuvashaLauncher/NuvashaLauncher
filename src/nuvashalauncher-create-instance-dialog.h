/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherCreateInstanceDialog: the single page dialog that creates a new
 * instance through nuvashalauncherInstanceManager. */

#ifndef nuvashalauncher_CREATE_INSTANCE_DIALOG_H
#define nuvashalauncher_CREATE_INSTANCE_DIALOG_H

#include <adwaita.h>

#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-settings.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_CREATE_INSTANCE_DIALOG (nuvashalauncher_create_instance_dialog_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherCreateInstanceDialog, nuvashalauncher_create_instance_dialog,
                      nuvashalauncher, CREATE_INSTANCE_DIALOG, AdwDialog)

/* Returns a floating dialog, like every other GTK widget. Presenting it
 * takes ownership of that reference. The dialog keeps its own
 * references on @manager and @settings until it is destroyed. The
 * Minecraft versions are fetched when the dialog opens. */
nuvashalauncherCreateInstanceDialog *nuvashalauncher_create_instance_dialog_new (nuvashalauncherInstanceManager *manager,
                                                         nuvashalauncherSettings        *settings);

G_END_DECLS

#endif /* nuvashalauncher_CREATE_INSTANCE_DIALOG_H */
