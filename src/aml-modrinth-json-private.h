/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_MODRINTH_JSON_PRIVATE_H
#define AML_MODRINTH_JSON_PRIVATE_H

#include "aml-modrinth.h"

JsonNode *aml_modrinth_root_node (const char *json, gssize length,
                                  GError **error);
gboolean aml_modrinth_string_array_has (GPtrArray *strings, const char *value);
gboolean aml_modrinth_loader_runs (const char *instance_loader,
                                   const char *version_loader);

#endif
