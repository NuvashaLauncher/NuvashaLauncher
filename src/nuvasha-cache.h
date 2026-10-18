/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_CACHE_H
#define nuvasha_CACHE_H

#include <gio/gio.h>
#include "nuvasha-task.h"

G_BEGIN_DECLS

char *nuvasha_cache_get_api_directory (void);
char *nuvasha_cache_get_files_directory (void);
gboolean nuvasha_cache_ensure_directory (const char *path, GError **error);

NuvashaTaskScope *nuvasha_cache_clear_api_async (GAsyncReadyCallback callback,
                                          gpointer            user_data);

/* Removes the contents of the global nuvasha cache tree. It never traverses
 * instance directories, so installed content and its metadata indexes stay
 * untouched. The callback receives a GTask boolean result. */
NuvashaTaskScope *nuvasha_cache_clear_all_async (GAsyncReadyCallback callback,
                                         gpointer            user_data);

G_END_DECLS

#endif /* nuvasha_CACHE_H */
