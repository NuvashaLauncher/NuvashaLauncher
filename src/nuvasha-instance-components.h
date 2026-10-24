/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_INSTANCE_COMPONENTS_H
#define nuvasha_INSTANCE_COMPONENTS_H

#include "nuvasha-instance.h"

G_BEGIN_DECLS

#define nuvasha_COMPONENT_MINECRAFT "net.minecraft"
#define nuvasha_COMPONENT_FABRIC "net.fabricmc.fabric-loader"
#define nuvasha_COMPONENT_FORGE "net.minecraftforge"
#define nuvasha_COMPONENT_NEOFORGE "net.neoforged.neoforge"
#define nuvasha_COMPONENT_QUILT "org.quiltmc.quilt-loader"

/* Component-style access over nuvasha's existing instance JSON. This keeps the
 * current format compatible while giving resolver and UI code stable IDs for
 * versioned components. */
char *nuvasha_instance_component_get_version (NuvashaInstance *instance,
                                          const char *component_id);
gboolean nuvasha_instance_component_set_version (NuvashaInstance *instance,
                                             const char *component_id,
                                             const char *version,
                                             GError **error);
const char *nuvasha_instance_component_id_for_loader (NuvashaLoader loader);

G_END_DECLS

#endif /* nuvasha_INSTANCE_COMPONENTS_H */
