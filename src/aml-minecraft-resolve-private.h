/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_MINECRAFT_RESOLVE_PRIVATE_H
#define AML_MINECRAFT_RESOLVE_PRIVATE_H

#include "aml-minecraft-resolve.h"

char *aml_resolve_safe_name (const char *id);
gboolean aml_resolve_ensure_directory (const char *path, GError **error);
gboolean aml_resolve_ensure_parent_directory (const char *path,
                                              GError **error);
AmlResolveFile *aml_resolve_file_new (const char *path, const char *url,
                                      const char *sha1, gboolean natives,
                                      GPtrArray *exclude);

#endif
