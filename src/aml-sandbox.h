/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_SANDBOX_H
#define AML_SANDBOX_H

#include <gio/gio.h>

#include "aml-minecraft-resolve.h"

G_BEGIN_DECLS

#define AML_SANDBOX_ERROR (aml_sandbox_error_quark ())

typedef enum {
  AML_SANDBOX_ERROR_UNAVAILABLE,
  AML_SANDBOX_ERROR_UNTRUSTED,
  AML_SANDBOX_ERROR_INVALID_PATH,
  AML_SANDBOX_ERROR_INVALID_DISPLAY
} AmlSandboxError;

GQuark aml_sandbox_error_quark (void);

/* Builds bwrap plus a rewritten, virtual-path version of @java_argv. The
 * returned array owns every argument and is suitable for GSubprocessLauncher.
 * The instance is writable; Java, libraries, assets, natives and version data
 * are read-only mounts. */
GPtrArray *aml_sandbox_build_arguments (GPtrArray              *java_argv,
                                        const char              *game_directory,
                                        const AmlResolvedVersion *resolved,
                                         const char              *java_executable,
                                         gboolean                 allow_network,
                                         int                     *seccomp_fd,
                                         int                      open_fd,
                                         int                     *broker_fd,
                                         int                     *broker_gio_fd,
                                         GError                 **error);

G_END_DECLS

#endif /* AML_SANDBOX_H */
