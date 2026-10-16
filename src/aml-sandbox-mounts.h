/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_SANDBOX_MOUNTS_H
#define AML_SANDBOX_MOUNTS_H

#include "aml-minecraft-resolve.h"

void aml_sandbox_add_system_mounts (GPtrArray *arguments);
void aml_sandbox_add_game_mounts (GPtrArray *arguments,
                                  const AmlResolvedVersion *resolved,
                                  const char *java_executable,
                                  const char *game_directory);

#endif /* AML_SANDBOX_MOUNTS_H */
