/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaCreateInstanceDialog: the single page dialog that creates a new
 * instance through NuvashaInstanceManager. */

#ifndef nuvasha_CREATE_INSTANCE_DIALOG_H
#define nuvasha_CREATE_INSTANCE_DIALOG_H

#include <adwaita.h>

#include "nuvasha-instance-manager.h"
#include "nuvasha-settings.h"

G_BEGIN_DECLS

#define nuvasha_TYPE_CREATE_INSTANCE_DIALOG (nuvasha_create_instance_dialog_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaCreateInstanceDialog, nuvasha_create_instance_dialog,
                      nuvasha, CREATE_INSTANCE_DIALOG, AdwDialog)

/* Returns a floating dialog, like every other GTK widget. Presenting it
 * takes ownership of that reference. The dialog keeps its own
 * references on @manager and @settings until it is destroyed. The
 * Minecraft versions are fetched when the dialog opens. */
NuvashaCreateInstanceDialog *nuvasha_create_instance_dialog_new (NuvashaInstanceManager *manager,
                                                         NuvashaSettings        *settings);

G_END_DECLS

#endif /* nuvasha_CREATE_INSTANCE_DIALOG_H */
