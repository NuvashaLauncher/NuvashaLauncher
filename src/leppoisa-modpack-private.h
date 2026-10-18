/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_MODPACK_PRIVATE_H
#define leppoisa_MODPACK_PRIVATE_H

#include "leppoisa-modpack.h"

#define leppoisa_MODPACK_FORMAT_VERSION 1

LeppoisaTaskScope *extract_overrides_async (const char *archive_path,
                                       const char *directory,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);
gboolean extract_overrides_finish (GAsyncResult *result, GError **error);
gboolean leppoisa_modpack_archive_entry_is_safe (const char *name, guint32 type,
                                            const char *hardlink,
                                            GError **error);

#endif /* leppoisa_MODPACK_PRIVATE_H */
