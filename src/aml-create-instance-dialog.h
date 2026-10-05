/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlCreateInstanceDialog: the single page dialog that creates a new
 * instance through AmlInstanceManager. */

#ifndef AML_CREATE_INSTANCE_DIALOG_H
#define AML_CREATE_INSTANCE_DIALOG_H

#include <adwaita.h>

#include "aml-instance-manager.h"
#include "aml-settings.h"

G_BEGIN_DECLS

#define AML_TYPE_CREATE_INSTANCE_DIALOG (aml_create_instance_dialog_get_type ())

G_DECLARE_FINAL_TYPE (AmlCreateInstanceDialog, aml_create_instance_dialog,
                      AML, CREATE_INSTANCE_DIALOG, AdwDialog)

/* Returns a floating dialog, like every other GTK widget. Presenting it
 * takes ownership of that reference. The dialog keeps its own
 * references on @manager and @settings until it is destroyed. The
 * Minecraft versions are fetched when the dialog opens. */
AmlCreateInstanceDialog *aml_create_instance_dialog_new (AmlInstanceManager *manager,
                                                         AmlSettings        *settings);

G_END_DECLS

#endif /* AML_CREATE_INSTANCE_DIALOG_H */
