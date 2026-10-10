/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_LOADERS_PRIVATE_H
#define AML_LOADERS_PRIVATE_H

#include "aml-loaders.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

const char *loader_meta (AmlLoader loader);
const char *loader_maven (AmlLoader loader, const char **coordinate);
gboolean give_libraries_their_downloads (JsonNode *root, GError **error);

G_END_DECLS

#endif /* AML_LOADERS_PRIVATE_H */
