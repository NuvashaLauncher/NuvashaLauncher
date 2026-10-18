/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_SANDBOX_MOUNTS_H
#define leppoisa_SANDBOX_MOUNTS_H

#include "leppoisa-minecraft-resolve.h"

void leppoisa_sandbox_add_system_mounts (GPtrArray *arguments);
void leppoisa_sandbox_add_game_mounts (GPtrArray *arguments,
                                  const LeppoisaResolvedVersion *resolved,
                                  const char *java_executable,
                                  const char *game_directory);

#endif /* leppoisa_SANDBOX_MOUNTS_H */
