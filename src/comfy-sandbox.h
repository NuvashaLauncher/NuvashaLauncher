/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_SANDBOX_H
#define comfy_SANDBOX_H

#include "comfy-minecraft-resolve.h"

G_BEGIN_DECLS

/* Wraps an already complete Java command in bubblewrap. The returned array
 * owns copies of every argument and is NULL terminated by the caller. */
GPtrArray *comfy_sandbox_wrap_arguments (GPtrArray              *command,
                                       const ComfyResolvedVersion *resolved,
                                       const char              *java_executable,
                                       const char              *game_directory,
                                       gboolean                 network_enabled,
                                       GError                 **error);

G_END_DECLS

#endif /* comfy_SANDBOX_H */
