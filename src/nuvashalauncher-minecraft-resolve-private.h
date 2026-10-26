/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MINECRAFT_RESOLVE_PRIVATE_H
#define nuvashalauncher_MINECRAFT_RESOLVE_PRIVATE_H

#include "nuvashalauncher-minecraft-resolve.h"

typedef struct ResolveRequest ResolveRequest;
typedef void (*ResolveStep) (ResolveRequest *request);

typedef struct
{
  ResolveRequest *request;
  ResolveStep next;
} PendingOperation;

struct ResolveRequest
{
  GTask *task;
  nuvashalauncherTaskScope *scope;
  nuvashalauncherVersionManifest *manifest;
  nuvashalauncherVersionDescription *description;
  char *version_id;
  char *data_dir;
  char *libraries_dir;
  char *versions_dir;
  char *assets_dir;
  char *indexes_dir;
  char *objects_dir;
  char *virtual_dir;
  char *natives_dir;
  char *client_jar;
  char *client_jar_override;
  char *logging_path;
  char *asset_index_id;
  char *asset_index_name;
  char *asset_base_url;
  nuvashalauncherRuleContext *context;
  nuvashalauncherResolveProgressFunc progress;
  gpointer progress_data;
  GCancellable *cancellable;
  GError *error;
  const char *stage;
  guint pending;
  guint done;
  guint total;
  GPtrArray *natives;
  GPtrArray *asset_hashes;
  GHashTable *asset_paths;
  gboolean asset_virtual;
  gboolean asset_map_to_resources;
};

char *nuvashalauncher_resolve_safe_name (const char *id);
gboolean nuvashalauncher_resolve_ensure_directory (const char *path, GError **error);
gboolean nuvashalauncher_resolve_ensure_parent_directory (const char *path,
                                              GError **error);
nuvashalauncherResolveFile *nuvashalauncher_resolve_file_new (const char *path, const char *url,
                                      const char *sha1, gboolean natives,
                                       GPtrArray *exclude);
nuvashalauncherResolvedVersion *build_result (ResolveRequest *request, GError **error);
void request_fail (ResolveRequest *, GError *);
void request_fail_and_wait (ResolveRequest *, GError *);
void report (ResolveRequest *);
void operation_started (ResolveRequest *);
void operation_finished (ResolveRequest *, ResolveStep, GError *);
void resolve_finish (ResolveRequest *);
void resolve_assets (ResolveRequest *);
void resolve_copies (ResolveRequest *);
void download_file (ResolveRequest *, ResolveStep, const char *, const char *, const char *);

#endif
