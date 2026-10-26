/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MODRINTH_JSON_PRIVATE_H
#define nuvashalauncher_MODRINTH_JSON_PRIVATE_H

#include "nuvashalauncher-modrinth.h"

JsonNode *nuvashalauncher_modrinth_root_node (const char *json, gssize length,
                                  GError **error);
gboolean nuvashalauncher_modrinth_string_array_has (GPtrArray *strings, const char *value);
gboolean nuvashalauncher_modrinth_loader_runs (const char *instance_loader,
                                   const char *version_loader);

#endif
