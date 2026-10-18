/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_SANDBOX_H
#define nuvasha_SANDBOX_H

#include "nuvasha-minecraft-resolve.h"

G_BEGIN_DECLS

/* Wraps an already complete Java command in bubblewrap. The returned array
 * owns copies of every argument and is NULL terminated by the caller. */
GPtrArray *nuvasha_sandbox_wrap_arguments (GPtrArray              *command,
                                       const NuvashaResolvedVersion *resolved,
                                       const char              *java_executable,
                                       const char              *game_directory,
                                       gboolean                 network_enabled,
                                       GError                 **error);

G_END_DECLS

#endif /* nuvasha_SANDBOX_H */
