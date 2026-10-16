/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_SANDBOX_H
#define AML_SANDBOX_H

#include "aml-minecraft-resolve.h"

G_BEGIN_DECLS

/* Wraps an already complete Java command in bubblewrap. The returned array
 * owns copies of every argument and is NULL terminated by the caller. */
GPtrArray *aml_sandbox_wrap_arguments (GPtrArray              *command,
                                       const AmlResolvedVersion *resolved,
                                       const char              *java_executable,
                                       const char              *game_directory,
                                       GError                 **error);

G_END_DECLS

#endif /* AML_SANDBOX_H */
