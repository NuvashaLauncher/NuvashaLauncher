/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MODRINTH_PRIVATE_H
#define nuvashalauncher_MODRINTH_PRIVATE_H

#include "nuvashalauncher-modrinth.h"

typedef void (*NuvashaLauncherModrinthAnswerFunc) (GTask *task, JsonNode *root);

typedef enum {
  nuvashalauncher_NOT_FOUND_IS_ERROR,
  nuvashalauncher_NOT_FOUND_IS_ABSENT
} NuvashaLauncherNotFoundRule;

typedef struct {
  NuvashaLauncherModrinthAnswerFunc answer;
  NuvashaLauncherNotFoundRule not_found;
  NuvashaLauncherTaskScope *scope;
  void *data;
  GDestroyNotify free_data;
} NuvashaLauncherModrinthRequest;

typedef NuvashaLauncherModrinthAnswerFunc AnswerFunc;
typedef NuvashaLauncherModrinthRequest Request;

char *nuvashalauncher_modrinth_string_array_json (const char *value);
char *nuvashalauncher_modrinth_loaders_json (const char *loader);

void nuvashalauncher_modrinth_request_return_error (GTask *task, GError *error);
void nuvashalauncher_modrinth_request_return_pointer (GTask *task, gpointer value,
                                                     GDestroyNotify free_value);
NuvashaLauncherTaskScope *nuvashalauncher_modrinth_request_new (
  GAsyncReadyCallback callback, gpointer user_data,
  NuvashaLauncherModrinthAnswerFunc answer,
  NuvashaLauncherNotFoundRule not_found, void *data,
  GDestroyNotify free_data);
void nuvashalauncher_modrinth_fail (GTask *task, GError *error);
gboolean nuvashalauncher_modrinth_error_is_not_found (const GError *error);
NuvashaLauncherTaskScope *nuvashalauncher_modrinth_get_async (
  const char *url, GAsyncReadyCallback callback, gpointer user_data,
  NuvashaLauncherModrinthAnswerFunc answer,
  NuvashaLauncherNotFoundRule not_found);
void nuvashalauncher_modrinth_json_ready (GObject *source, GAsyncResult *result,
                                          gpointer user_data);

NuvashaLauncherTaskScope *nuvashalauncher_modrinth_updates_by_hashes_url_async (
  const char                 *url,
  NuvashaLauncherContentType  type,
  GPtrArray                  *hashes,
  const char                 *minecraft_version,
  const char                 *loader,
  GAsyncReadyCallback         callback,
  gpointer                    user_data);

#endif
