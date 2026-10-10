/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_CONTENT_INDEX_H
#define AML_CONTENT_INDEX_H

#include <gio/gio.h>

#include "aml-modrinth.h"

G_BEGIN_DECLS

#define AML_CONTENT_INDEX_ERROR (aml_content_index_error_quark ())

typedef enum {
  AML_CONTENT_INDEX_ERROR_INVALID
} AmlContentIndexError;

typedef struct {
  char *filename;
  char *hash;
  char *hash512;
  char *download_url;
  char *env_client;
  char *env_server;
  char *provider;
  char *project_id;
  char *project_name;
  char *icon_url;
  char *version_id;
  char *version_name;
  GPtrArray *required_dependencies;
  GPtrArray *optional_dependencies;
  GPtrArray *incompatible_dependencies;
} AmlContentIndexEntry;

void aml_content_index_entry_free (AmlContentIndexEntry *entry);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlContentIndexEntry,
                               aml_content_index_entry_free)

GQuark aml_content_index_error_quark (void);

void aml_content_index_load_async (const char *instance_directory,
                                   AmlContentType type,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);
GPtrArray *aml_content_index_load_finish (GAsyncResult *result,
                                          GError **error);

void aml_content_index_update_async (const char *instance_directory,
                                     AmlContentType type,
                                     AmlContentIndexEntry *entry,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data);

void aml_content_index_remove_async (const char *instance_directory,
                                     AmlContentType type,
                                     const char *filename,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data);

G_END_DECLS

#endif /* AML_CONTENT_INDEX_H */
