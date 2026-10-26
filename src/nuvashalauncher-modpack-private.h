/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MODPACK_PRIVATE_H
#define nuvashalauncher_MODPACK_PRIVATE_H

#include "nuvashalauncher-modpack.h"

#define nuvashalauncher_MODPACK_FORMAT_VERSION 1

nuvashalauncherTaskScope *extract_overrides_async (const char *archive_path,
                                       const char *directory,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);
gboolean extract_overrides_finish (GAsyncResult *result, GError **error);
gboolean nuvashalauncher_modpack_archive_entry_is_safe (const char *name, guint32 type,
                                            const char *hardlink,
                                            GError **error);

#endif /* nuvashalauncher_MODPACK_PRIVATE_H */
