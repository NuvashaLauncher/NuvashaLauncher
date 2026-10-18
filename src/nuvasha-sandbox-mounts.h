/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_SANDBOX_MOUNTS_H
#define nuvasha_SANDBOX_MOUNTS_H

#include "nuvasha-minecraft-resolve.h"

void nuvasha_sandbox_add_system_mounts (GPtrArray *arguments);
void nuvasha_sandbox_add_game_mounts (GPtrArray *arguments,
                                  const NuvashaResolvedVersion *resolved,
                                  const char *java_executable,
                                  const char *game_directory);

#endif /* nuvasha_SANDBOX_MOUNTS_H */
