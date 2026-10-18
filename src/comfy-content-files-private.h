/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_CONTENT_FILES_PRIVATE_H
#define comfy_CONTENT_FILES_PRIVATE_H

#include "comfy-content-page.h"

void comfy_content_apply_local_metadata (ComfyContentFile *file,
                                       ComfyContentType type);
gboolean comfy_content_load_local_icon (ComfyContentFile *file);
ComfyTaskScope *comfy_content_load_local_icon_async (ComfyContentFile *file,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
char *comfy_content_load_local_icon_finish (GAsyncResult *result,
                                          GError **error);
void comfy_content_merge_dependencies (GPtrArray **destination,
                                     GPtrArray *source);

#endif
