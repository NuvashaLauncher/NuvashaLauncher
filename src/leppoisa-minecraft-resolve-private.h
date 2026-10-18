/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_MINECRAFT_RESOLVE_PRIVATE_H
#define leppoisa_MINECRAFT_RESOLVE_PRIVATE_H

#include "leppoisa-minecraft-resolve.h"

char *leppoisa_resolve_safe_name (const char *id);
gboolean leppoisa_resolve_ensure_directory (const char *path, GError **error);
gboolean leppoisa_resolve_ensure_parent_directory (const char *path,
                                              GError **error);
LeppoisaResolveFile *leppoisa_resolve_file_new (const char *path, const char *url,
                                      const char *sha1, gboolean natives,
                                      GPtrArray *exclude);

#endif
