/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_CACHE_H
#define comfy_CACHE_H

#include <gio/gio.h>
#include "comfy-task.h"

G_BEGIN_DECLS

char *comfy_cache_get_api_directory (void);
char *comfy_cache_get_files_directory (void);
gboolean comfy_cache_ensure_directory (const char *path, GError **error);

ComfyTaskScope *comfy_cache_clear_api_async (GAsyncReadyCallback callback,
                                          gpointer            user_data);

/* Removes the contents of the global comfy cache tree. It never traverses
 * instance directories, so installed content and its metadata indexes stay
 * untouched. The callback receives a GTask boolean result. */
ComfyTaskScope *comfy_cache_clear_all_async (GAsyncReadyCallback callback,
                                         gpointer            user_data);

G_END_DECLS

#endif /* comfy_CACHE_H */
