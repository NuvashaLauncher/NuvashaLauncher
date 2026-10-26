/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_INSTANCE_COMPONENTS_H
#define nuvashalauncher_INSTANCE_COMPONENTS_H

#include "nuvashalauncher-instance.h"

G_BEGIN_DECLS

#define nuvashalauncher_COMPONENT_MINECRAFT "net.minecraft"
#define nuvashalauncher_COMPONENT_FABRIC "net.fabricmc.fabric-loader"
#define nuvashalauncher_COMPONENT_FORGE "net.minecraftforge"
#define nuvashalauncher_COMPONENT_NEOFORGE "net.neoforged.neoforge"
#define nuvashalauncher_COMPONENT_QUILT "org.quiltmc.quilt-loader"

/* Component-style access over nuvashalauncher's existing instance JSON. This keeps the
 * current format compatible while giving resolver and UI code stable IDs for
 * versioned components. */
char *nuvashalauncher_instance_component_get_version (nuvashalauncherInstance *instance,
                                          const char *component_id);
gboolean nuvashalauncher_instance_component_set_version (nuvashalauncherInstance *instance,
                                             const char *component_id,
                                             const char *version,
                                             GError **error);
const char *nuvashalauncher_instance_component_id_for_loader (nuvashalauncherLoader loader);

G_END_DECLS

#endif /* nuvashalauncher_INSTANCE_COMPONENTS_H */
