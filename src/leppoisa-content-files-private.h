/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_CONTENT_FILES_PRIVATE_H
#define leppoisa_CONTENT_FILES_PRIVATE_H

#include "leppoisa-content-page.h"

void leppoisa_content_apply_local_metadata (LeppoisaContentFile *file,
                                       LeppoisaContentType type);
gboolean leppoisa_content_load_local_icon (LeppoisaContentFile *file);
LeppoisaTaskScope *leppoisa_content_load_local_icon_async (LeppoisaContentFile *file,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
char *leppoisa_content_load_local_icon_finish (GAsyncResult *result,
                                          GError **error);
void leppoisa_content_merge_dependencies (GPtrArray **destination,
                                     GPtrArray *source);

#endif
