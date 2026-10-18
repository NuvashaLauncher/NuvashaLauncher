/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaCreateInstanceDialog: the single page dialog that creates a new
 * instance through LeppoisaInstanceManager. */

#ifndef leppoisa_CREATE_INSTANCE_DIALOG_H
#define leppoisa_CREATE_INSTANCE_DIALOG_H

#include <adwaita.h>

#include "leppoisa-instance-manager.h"
#include "leppoisa-settings.h"

G_BEGIN_DECLS

#define leppoisa_TYPE_CREATE_INSTANCE_DIALOG (leppoisa_create_instance_dialog_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaCreateInstanceDialog, leppoisa_create_instance_dialog,
                      leppoisa, CREATE_INSTANCE_DIALOG, AdwDialog)

/* Returns a floating dialog, like every other GTK widget. Presenting it
 * takes ownership of that reference. The dialog keeps its own
 * references on @manager and @settings until it is destroyed. The
 * Minecraft versions are fetched when the dialog opens. */
LeppoisaCreateInstanceDialog *leppoisa_create_instance_dialog_new (LeppoisaInstanceManager *manager,
                                                         LeppoisaSettings        *settings);

G_END_DECLS

#endif /* leppoisa_CREATE_INSTANCE_DIALOG_H */
