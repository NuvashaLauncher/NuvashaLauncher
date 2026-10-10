/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_INSTANCE_COMPONENTS_H
#define AML_INSTANCE_COMPONENTS_H

#include "aml-instance.h"

G_BEGIN_DECLS

#define AML_COMPONENT_MINECRAFT "net.minecraft"
#define AML_COMPONENT_FABRIC "net.fabricmc.fabric-loader"
#define AML_COMPONENT_FORGE "net.minecraftforge"
#define AML_COMPONENT_NEOFORGE "net.neoforged.neoforge"
#define AML_COMPONENT_QUILT "org.quiltmc.quilt-loader"

/* Component-style access over AML's existing instance JSON. This keeps the
 * current format compatible while allowing resolver and UI code to refer to
 * versioned components by stable IDs instead of fields. */
char *aml_instance_component_get_version (AmlInstance *instance,
                                          const char *component_id);
gboolean aml_instance_component_set_version (AmlInstance *instance,
                                             const char *component_id,
                                             const char *version,
                                             GError **error);
const char *aml_instance_component_id_for_loader (AmlLoader loader);

G_END_DECLS

#endif /* AML_INSTANCE_COMPONENTS_H */
