/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_INSTANCE_COMPONENTS_H
#define comfy_INSTANCE_COMPONENTS_H

#include "comfy-instance.h"

G_BEGIN_DECLS

#define comfy_COMPONENT_MINECRAFT "net.minecraft"
#define comfy_COMPONENT_FABRIC "net.fabricmc.fabric-loader"
#define comfy_COMPONENT_FORGE "net.minecraftforge"
#define comfy_COMPONENT_NEOFORGE "net.neoforged.neoforge"
#define comfy_COMPONENT_QUILT "org.quiltmc.quilt-loader"

/* Component-style access over comfy's existing instance JSON. This keeps the
 * current format compatible while allowing resolver and UI code to refer to
 * versioned components by stable IDs instead of fields. */
char *comfy_instance_component_get_version (ComfyInstance *instance,
                                          const char *component_id);
gboolean comfy_instance_component_set_version (ComfyInstance *instance,
                                             const char *component_id,
                                             const char *version,
                                             GError **error);
const char *comfy_instance_component_id_for_loader (ComfyLoader loader);

G_END_DECLS

#endif /* comfy_INSTANCE_COMPONENTS_H */
