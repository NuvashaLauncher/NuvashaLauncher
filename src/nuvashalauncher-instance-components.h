/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_INSTANCE_COMPONENTS_H
#define NUVASHALAUNCHER_INSTANCE_COMPONENTS_H

#include "nuvashalauncher-instance.h"

G_BEGIN_DECLS

#define NUVASHALAUNCHER_COMPONENT_MINECRAFT "net.minecraft"
#define NUVASHALAUNCHER_COMPONENT_FABRIC "net.fabricmc.fabric-loader"
#define NUVASHALAUNCHER_COMPONENT_FORGE "net.minecraftforge"
#define NUVASHALAUNCHER_COMPONENT_NEOFORGE "net.neoforged.neoforge"
#define NUVASHALAUNCHER_COMPONENT_QUILT "org.quiltmc.quilt-loader"

/* Component-style access over nuvashalauncher's existing instance JSON. This keeps the
 * current format compatible while giving resolver and UI code stable IDs for
 * versioned components. */
char *nuvashalauncher_instance_component_get_version (NuvashaLauncherInstance *instance,
                                          const char *component_id);
gboolean nuvashalauncher_instance_component_set_version (NuvashaLauncherInstance *instance,
                                             const char *component_id,
                                             const char *version,
                                             GError **error);
const char *nuvashalauncher_instance_component_id_for_loader (NuvashaLauncherLoader loader);

G_END_DECLS

#endif /* NUVASHALAUNCHER_INSTANCE_COMPONENTS_H */
