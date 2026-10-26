/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_CACHE_H
#define nuvashalauncher_CACHE_H

#include <gio/gio.h>
#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

char *nuvashalauncher_cache_get_api_directory (void);
char *nuvashalauncher_cache_get_files_directory (void);
gboolean nuvashalauncher_cache_ensure_directory (const char *path, GError **error);

nuvashalauncherTaskScope *nuvashalauncher_cache_clear_api_async (GAsyncReadyCallback callback,
                                          gpointer            user_data);

/* Removes the contents of the global nuvashalauncher cache tree. It never traverses
 * instance directories, so installed content and its metadata indexes stay
 * untouched. The callback receives a GTask boolean result. */
nuvashalauncherTaskScope *nuvashalauncher_cache_clear_all_async (GAsyncReadyCallback callback,
                                         gpointer            user_data);

G_END_DECLS

#endif /* nuvashalauncher_CACHE_H */
