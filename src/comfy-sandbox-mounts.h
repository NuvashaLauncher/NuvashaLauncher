/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_SANDBOX_MOUNTS_H
#define comfy_SANDBOX_MOUNTS_H

#include "comfy-minecraft-resolve.h"

void comfy_sandbox_add_system_mounts (GPtrArray *arguments);
void comfy_sandbox_add_game_mounts (GPtrArray *arguments,
                                  const ComfyResolvedVersion *resolved,
                                  const char *java_executable,
                                  const char *game_directory);

#endif /* comfy_SANDBOX_MOUNTS_H */
