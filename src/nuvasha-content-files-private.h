/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_CONTENT_FILES_PRIVATE_H
#define nuvasha_CONTENT_FILES_PRIVATE_H

#include "nuvasha-content-page.h"

void nuvasha_content_apply_local_metadata (NuvashaContentFile *file,
                                       NuvashaContentType type);
gboolean nuvasha_content_load_local_icon (NuvashaContentFile *file);
NuvashaTaskScope *nuvasha_content_load_local_icon_async (NuvashaContentFile *file,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
char *nuvasha_content_load_local_icon_finish (GAsyncResult *result,
                                          GError **error);
void nuvasha_content_merge_dependencies (GPtrArray **destination,
                                     GPtrArray *source);

#endif
