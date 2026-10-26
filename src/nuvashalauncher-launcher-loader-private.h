/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_LAUNCHER_LOADER_PRIVATE_H
#define nuvashalauncher_LAUNCHER_LOADER_PRIVATE_H

#include "nuvashalauncher-launcher-private.h"

typedef struct {
  nuvashalauncher *launcher; /* the reference this work holds */
  nuvashalauncherVersionManifest *manifest;
  nuvashalauncherLoader loader;
  nuvashalauncherVersionDescription *profile; /* the document the loader adds */
  char *game_version;
  char *loader_version;

  /* The operation this request is waiting on, released with it. */
  nuvashalauncherTaskScope *operation;
} LoaderRequest;

void loader_request_free (LoaderRequest *request);
void on_java_for_the_install (GObject *source, GAsyncResult *result,
                              gpointer user_data);

#endif /* nuvashalauncher_LAUNCHER_LOADER_PRIVATE_H */
