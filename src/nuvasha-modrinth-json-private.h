/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_MODRINTH_JSON_PRIVATE_H
#define nuvasha_MODRINTH_JSON_PRIVATE_H

#include "nuvasha-modrinth.h"

JsonNode *nuvasha_modrinth_root_node (const char *json, gssize length,
                                  GError **error);
gboolean nuvasha_modrinth_string_array_has (GPtrArray *strings, const char *value);
gboolean nuvasha_modrinth_loader_runs (const char *instance_loader,
                                   const char *version_loader);

#endif
