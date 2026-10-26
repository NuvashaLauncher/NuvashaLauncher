/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MODRINTH_PRIVATE_H
#define nuvashalauncher_MODRINTH_PRIVATE_H

#include "nuvashalauncher-modrinth.h"

typedef void (*nuvashalauncherModrinthAnswerFunc) (GTask *task, JsonNode *root);

typedef enum {
  nuvashalauncher_NOT_FOUND_IS_ERROR,
  nuvashalauncher_NOT_FOUND_IS_ABSENT
} nuvashalauncherNotFoundRule;

typedef struct {
  nuvashalauncherModrinthAnswerFunc answer;
  nuvashalauncherNotFoundRule not_found;
  nuvashalauncherTaskScope *scope;
  void *data;
  GDestroyNotify free_data;
} nuvashalauncherModrinthRequest;

typedef nuvashalauncherModrinthAnswerFunc AnswerFunc;
typedef nuvashalauncherModrinthRequest Request;

char *nuvashalauncher_modrinth_string_array_json (const char *value);
char *nuvashalauncher_modrinth_loaders_json (const char *loader);

void nuvashalauncher_modrinth_request_return_error (GTask *task, GError *error);
void nuvashalauncher_modrinth_request_return_pointer (GTask *task, gpointer value,
                                                     GDestroyNotify free_value);
nuvashalauncherTaskScope *nuvashalauncher_modrinth_request_new (
  GAsyncReadyCallback callback, gpointer user_data,
  nuvashalauncherModrinthAnswerFunc answer,
  nuvashalauncherNotFoundRule not_found, void *data,
  GDestroyNotify free_data);
void nuvashalauncher_modrinth_fail (GTask *task, GError *error);
gboolean nuvashalauncher_modrinth_error_is_not_found (const GError *error);
nuvashalauncherTaskScope *nuvashalauncher_modrinth_get_async (
  const char *url, GAsyncReadyCallback callback, gpointer user_data,
  nuvashalauncherModrinthAnswerFunc answer,
  nuvashalauncherNotFoundRule not_found);
void nuvashalauncher_modrinth_json_ready (GObject *source, GAsyncResult *result,
                                          gpointer user_data);

#endif
