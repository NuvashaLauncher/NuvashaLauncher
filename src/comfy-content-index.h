/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_CONTENT_INDEX_H
#define comfy_CONTENT_INDEX_H

#include <gio/gio.h>

#include "comfy-modrinth.h"
#include "comfy-task.h"

G_BEGIN_DECLS

#define comfy_CONTENT_INDEX_ERROR (comfy_content_index_error_quark ())

typedef enum {
  comfy_CONTENT_INDEX_ERROR_INVALID
} ComfyContentIndexError;

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
} ComfyContentIndexEntry;

void comfy_content_index_entry_free (ComfyContentIndexEntry *entry);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyContentIndexEntry,
                               comfy_content_index_entry_free)

GQuark comfy_content_index_error_quark (void);

char *comfy_content_index_entry_path (const char *instance_directory,
                                    ComfyContentType type,
                                    const char *filename);
GPtrArray *comfy_content_index_load_sync (const char *instance_directory,
                                        ComfyContentType type,
                                        GError **error);

/* (transfer full) the operation. The callback never runs before the call
 * that starts it returns, so the handle can be stored here and released
 * from the callback. */
ComfyTaskScope *comfy_content_index_load_async (const char *instance_directory,
                                           ComfyContentType type,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
GPtrArray *comfy_content_index_load_finish (GAsyncResult *result,
                                          GError **error);

ComfyTaskScope *comfy_content_index_update_async (const char *instance_directory,
                                              ComfyContentType type,
                                              ComfyContentIndexEntry *entry,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

ComfyTaskScope *comfy_content_index_remove_async (const char *instance_directory,
                                              ComfyContentType type,
                                              const char *filename,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

/* Completes update_async() and remove_async(). */
gboolean comfy_content_index_modify_finish (GAsyncResult *result,
                                          GError **error);

G_END_DECLS

#endif /* comfy_CONTENT_INDEX_H */
