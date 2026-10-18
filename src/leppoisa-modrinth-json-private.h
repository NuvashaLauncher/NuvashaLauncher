/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_MODRINTH_JSON_PRIVATE_H
#define leppoisa_MODRINTH_JSON_PRIVATE_H

#include "leppoisa-modrinth.h"

JsonNode *leppoisa_modrinth_root_node (const char *json, gssize length,
                                  GError **error);
gboolean leppoisa_modrinth_string_array_has (GPtrArray *strings, const char *value);
gboolean leppoisa_modrinth_loader_runs (const char *instance_loader,
                                   const char *version_loader);

#endif
