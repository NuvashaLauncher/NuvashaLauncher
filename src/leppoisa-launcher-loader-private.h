/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_LAUNCHER_LOADER_PRIVATE_H
#define leppoisa_LAUNCHER_LOADER_PRIVATE_H

#include "leppoisa-launcher-private.h"

typedef struct {
  LeppoisaLauncher *launcher; /* the reference this work holds */
  LeppoisaVersionManifest *manifest;
  LeppoisaLoader loader;
  LeppoisaVersionDescription *profile; /* the document the loader adds */
  char *game_version;
  char *loader_version;

  /* The operation this request is waiting on, released with it. */
  LeppoisaTaskScope *operation;
} LoaderRequest;

void loader_request_free (LoaderRequest *request);
void on_java_for_the_install (GObject *source, GAsyncResult *result,
                              gpointer user_data);

#endif /* leppoisa_LAUNCHER_LOADER_PRIVATE_H */
