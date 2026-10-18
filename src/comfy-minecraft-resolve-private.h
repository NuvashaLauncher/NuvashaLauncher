/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_MINECRAFT_RESOLVE_PRIVATE_H
#define comfy_MINECRAFT_RESOLVE_PRIVATE_H

#include "comfy-minecraft-resolve.h"

char *comfy_resolve_safe_name (const char *id);
gboolean comfy_resolve_ensure_directory (const char *path, GError **error);
gboolean comfy_resolve_ensure_parent_directory (const char *path,
                                              GError **error);
ComfyResolveFile *comfy_resolve_file_new (const char *path, const char *url,
                                      const char *sha1, gboolean natives,
                                      GPtrArray *exclude);

#endif
