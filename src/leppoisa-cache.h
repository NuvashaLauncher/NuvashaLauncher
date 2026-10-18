/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_CACHE_H
#define leppoisa_CACHE_H

#include <gio/gio.h>
#include "leppoisa-task.h"

G_BEGIN_DECLS

char *leppoisa_cache_get_api_directory (void);
char *leppoisa_cache_get_files_directory (void);
gboolean leppoisa_cache_ensure_directory (const char *path, GError **error);

LeppoisaTaskScope *leppoisa_cache_clear_api_async (GAsyncReadyCallback callback,
                                          gpointer            user_data);

/* Removes the contents of the global leppoisa cache tree. It never traverses
 * instance directories, so installed content and its metadata indexes stay
 * untouched. The callback receives a GTask boolean result. */
LeppoisaTaskScope *leppoisa_cache_clear_all_async (GAsyncReadyCallback callback,
                                         gpointer            user_data);

G_END_DECLS

#endif /* leppoisa_CACHE_H */
