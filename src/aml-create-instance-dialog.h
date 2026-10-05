/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlCreateInstanceDialog: the single page dialog that creates a new
 * instance through AmlInstanceManager. */

#ifndef AML_CREATE_INSTANCE_DIALOG_H
#define AML_CREATE_INSTANCE_DIALOG_H

#include <adwaita.h>

#include "aml-instance-manager.h"

G_BEGIN_DECLS

#define AML_TYPE_CREATE_INSTANCE_DIALOG (aml_create_instance_dialog_get_type ())

G_DECLARE_FINAL_TYPE (AmlCreateInstanceDialog, aml_create_instance_dialog,
                      AML, CREATE_INSTANCE_DIALOG, AdwDialog)

/* Returns a floating dialog, like every other GTK widget. Presenting it
 * takes ownership of that reference. The dialog keeps its own reference
 * on @manager until it is destroyed. */
AmlCreateInstanceDialog *aml_create_instance_dialog_new (AmlInstanceManager *manager);

G_END_DECLS

#endif /* AML_CREATE_INSTANCE_DIALOG_H */
