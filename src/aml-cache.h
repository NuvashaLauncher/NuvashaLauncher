/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_CACHE_H
#define AML_CACHE_H

#include <gio/gio.h>
#include "aml-task.h"

G_BEGIN_DECLS

char *aml_cache_get_api_directory (void);
char *aml_cache_get_files_directory (void);
gboolean aml_cache_ensure_directory (const char *path, GError **error);

AmlTaskScope *aml_cache_clear_api_async (GAsyncReadyCallback callback,
                                          gpointer            user_data);

/* Removes the contents of the global AML cache tree. It never traverses
 * instance directories, so installed content and its metadata indexes stay
 * untouched. The callback receives a GTask boolean result. */
AmlTaskScope *aml_cache_clear_all_async (GAsyncReadyCallback callback,
                                         gpointer            user_data);

G_END_DECLS

#endif /* AML_CACHE_H */
