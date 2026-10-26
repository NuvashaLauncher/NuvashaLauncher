/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MINECRAFT_RESOLVE_PRIVATE_H
#define nuvashalauncher_MINECRAFT_RESOLVE_PRIVATE_H

#include "nuvashalauncher-minecraft-resolve.h"

char *nuvashalauncher_resolve_safe_name (const char *id);
gboolean nuvashalauncher_resolve_ensure_directory (const char *path, GError **error);
gboolean nuvashalauncher_resolve_ensure_parent_directory (const char *path,
                                              GError **error);
nuvashalauncherResolveFile *nuvashalauncher_resolve_file_new (const char *path, const char *url,
                                      const char *sha1, gboolean natives,
                                      GPtrArray *exclude);

#endif
