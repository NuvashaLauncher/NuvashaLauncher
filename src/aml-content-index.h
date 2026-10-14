/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_CONTENT_INDEX_H
#define AML_CONTENT_INDEX_H

#include <gio/gio.h>

#include "aml-modrinth.h"
#include "aml-task.h"

G_BEGIN_DECLS

#define AML_CONTENT_INDEX_ERROR (aml_content_index_error_quark ())

typedef enum {
  AML_CONTENT_INDEX_ERROR_INVALID
} AmlContentIndexError;

typedef struct {
  char *filename;
  guint64 size;
  gint64 mtime_usec;
  char *hash;
  char *hash512;
  char *download_url;
  char *env_client;
  char *env_server;
  char *provider;
  char *project_id;
  char *project_name;
  char *icon_url;
  gboolean metadata_checked;
  char *local_icon_member;
  char *local_icon_path;
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

char *aml_content_index_entry_path (const char *instance_directory,
                                    AmlContentType type,
                                    const char *filename);
GPtrArray *aml_content_index_load_sync (const char *instance_directory,
                                        AmlContentType type,
                                        GError **error);

/* (transfer full) the operation. The callback never runs before the call
 * that starts it returns, so the handle can be stored here and released
 * from the callback. */
AmlTaskScope *aml_content_index_load_async (const char *instance_directory,
                                           AmlContentType type,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
GPtrArray *aml_content_index_load_finish (GAsyncResult *result,
                                          GError **error);

AmlTaskScope *aml_content_index_update_async (const char *instance_directory,
                                              AmlContentType type,
                                              AmlContentIndexEntry *entry,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

AmlTaskScope *aml_content_index_remove_async (const char *instance_directory,
                                              AmlContentType type,
                                              const char *filename,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

/* Completes update_async() and remove_async(). */
gboolean aml_content_index_modify_finish (GAsyncResult *result,
                                          GError **error);

G_END_DECLS

#endif /* AML_CONTENT_INDEX_H */
