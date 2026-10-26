/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_CONTENT_FILES_PRIVATE_H
#define nuvashalauncher_CONTENT_FILES_PRIVATE_H

#include "nuvashalauncher-content-page.h"

void nuvashalauncher_content_apply_local_metadata (nuvashalauncherContentFile *file,
                                       nuvashalauncherContentType type);
gboolean nuvashalauncher_content_load_local_icon (nuvashalauncherContentFile *file);
nuvashalauncherTaskScope *nuvashalauncher_content_load_local_icon_async (nuvashalauncherContentFile *file,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
char *nuvashalauncher_content_load_local_icon_finish (GAsyncResult *result,
                                          GError **error);
void nuvashalauncher_content_merge_dependencies (GPtrArray **destination,
                                     GPtrArray *source);

#endif
