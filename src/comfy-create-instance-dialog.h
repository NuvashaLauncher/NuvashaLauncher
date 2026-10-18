/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyCreateInstanceDialog: the single page dialog that creates a new
 * instance through ComfyInstanceManager. */

#ifndef comfy_CREATE_INSTANCE_DIALOG_H
#define comfy_CREATE_INSTANCE_DIALOG_H

#include <adwaita.h>

#include "comfy-instance-manager.h"
#include "comfy-settings.h"

G_BEGIN_DECLS

#define comfy_TYPE_CREATE_INSTANCE_DIALOG (comfy_create_instance_dialog_get_type ())

G_DECLARE_FINAL_TYPE (ComfyCreateInstanceDialog, comfy_create_instance_dialog,
                      comfy, CREATE_INSTANCE_DIALOG, AdwDialog)

/* Returns a floating dialog, like every other GTK widget. Presenting it
 * takes ownership of that reference. The dialog keeps its own
 * references on @manager and @settings until it is destroyed. The
 * Minecraft versions are fetched when the dialog opens. */
ComfyCreateInstanceDialog *comfy_create_instance_dialog_new (ComfyInstanceManager *manager,
                                                         ComfySettings        *settings);

G_END_DECLS

#endif /* comfy_CREATE_INSTANCE_DIALOG_H */
