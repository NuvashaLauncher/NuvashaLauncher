/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_LOADERS_PRIVATE_H
#define nuvasha_LOADERS_PRIVATE_H

#include "nuvasha-loaders.h"
#include <json-glib/json-glib.h>
#include "nuvasha-task-private.h"

G_BEGIN_DECLS

const char *loader_meta (NuvashaLoader loader);
const char *loader_maven (NuvashaLoader loader, const char **coordinate);
gboolean give_libraries_their_downloads (JsonNode *root, GError **error);

char *nuvasha_loader_jar_manifest_attribute (const char *path, const char *name);
gboolean nuvasha_loader_write_jar_manifest_attribute (const char *path,
                                                  const char *name,
                                                  const char *value,
                                                  GError **error);
char *nuvasha_loader_jar_main_class (const char *path, GError **error);
gboolean nuvasha_loader_ensure_directory (const char *path, GError **error);
gboolean nuvasha_loader_write_install_file (const char *path, const void *data,
                                         gsize size, GError **error);
GBytes *nuvasha_loader_read_zip_entry (const char *path, const char *entry,
                                   GError **error);

G_END_DECLS

typedef enum {
  FILE_FETCH,
  FILE_UNPACK
} InstallFileKind;

typedef struct {
  InstallFileKind kind;
  char *url;
  char *entry;
  char *path;
  char *sha1;
  gboolean optional;
} InstallFile;

typedef enum {
  STAGE_CLIENT_JAR,
  STAGE_INSTALLER,
  STAGE_PROFILE,
  STAGE_VARIABLES,
  STAGE_RUN,
  STAGE_DONE
} InstallStage;

typedef struct {
  GTask *task;
  NuvashaTaskScope *scope;
  NuvashaLoader loader;
  InstallStage stage;
  char *data_dir;
  char *game_version;
  char *loader_version;
  char *java;
  char *root;
  char *libraries_dir;
  char *installer;
  char *client_jar;
  char *patched_jar;
  JsonParser *profile;
  NuvashaVersionDescription *description;
  GPtrArray *processors;
  guint index;
  char *main_class;
  GHashTable *variables;
  GPtrArray *pending;
  guint done;
  NuvashaLoadersProgressFunc progress;
  gpointer progress_data;
} InstallRequest;

void install_failed (InstallRequest *, GError *);
void report_progress (InstallRequest *);
void queue_artifact (InstallRequest *, const char *, gboolean);
void queue_unpacked (InstallRequest *, const char *);
void drain_pending (InstallRequest *);
void install_step_done (InstallRequest *);
char *artifact_path (InstallRequest *, const char *);
JsonObject *install_profile (InstallRequest *);
void stage_processor (InstallRequest *);
void stage_run (InstallRequest *);

#endif /* nuvasha_LOADERS_PRIVATE_H */
