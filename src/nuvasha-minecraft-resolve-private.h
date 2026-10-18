/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_MINECRAFT_RESOLVE_PRIVATE_H
#define nuvasha_MINECRAFT_RESOLVE_PRIVATE_H

#include "nuvasha-minecraft-resolve.h"

char *nuvasha_resolve_safe_name (const char *id);
gboolean nuvasha_resolve_ensure_directory (const char *path, GError **error);
gboolean nuvasha_resolve_ensure_parent_directory (const char *path,
                                              GError **error);
NuvashaResolveFile *nuvasha_resolve_file_new (const char *path, const char *url,
                                      const char *sha1, gboolean natives,
                                      GPtrArray *exclude);

#endif
