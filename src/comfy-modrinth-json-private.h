/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_MODRINTH_JSON_PRIVATE_H
#define comfy_MODRINTH_JSON_PRIVATE_H

#include "comfy-modrinth.h"

JsonNode *comfy_modrinth_root_node (const char *json, gssize length,
                                  GError **error);
gboolean comfy_modrinth_string_array_has (GPtrArray *strings, const char *value);
gboolean comfy_modrinth_loader_runs (const char *instance_loader,
                                   const char *version_loader);

#endif
