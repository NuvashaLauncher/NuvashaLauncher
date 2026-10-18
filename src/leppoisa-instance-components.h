/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_INSTANCE_COMPONENTS_H
#define leppoisa_INSTANCE_COMPONENTS_H

#include "leppoisa-instance.h"

G_BEGIN_DECLS

#define leppoisa_COMPONENT_MINECRAFT "net.minecraft"
#define leppoisa_COMPONENT_FABRIC "net.fabricmc.fabric-loader"
#define leppoisa_COMPONENT_FORGE "net.minecraftforge"
#define leppoisa_COMPONENT_NEOFORGE "net.neoforged.neoforge"
#define leppoisa_COMPONENT_QUILT "org.quiltmc.quilt-loader"

/* Component-style access over leppoisa's existing instance JSON. This keeps the
 * current format compatible while allowing resolver and UI code to refer to
 * versioned components by stable IDs instead of fields. */
char *leppoisa_instance_component_get_version (LeppoisaInstance *instance,
                                          const char *component_id);
gboolean leppoisa_instance_component_set_version (LeppoisaInstance *instance,
                                             const char *component_id,
                                             const char *version,
                                             GError **error);
const char *leppoisa_instance_component_id_for_loader (LeppoisaLoader loader);

G_END_DECLS

#endif /* leppoisa_INSTANCE_COMPONENTS_H */
