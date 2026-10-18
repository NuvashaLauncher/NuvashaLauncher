/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_LAUNCHER_LOADER_PRIVATE_H
#define nuvasha_LAUNCHER_LOADER_PRIVATE_H

#include "nuvasha-launcher-private.h"

typedef struct {
  NuvashaLauncher *launcher; /* the reference this work holds */
  NuvashaVersionManifest *manifest;
  NuvashaLoader loader;
  NuvashaVersionDescription *profile; /* the document the loader adds */
  char *game_version;
  char *loader_version;

  /* The operation this request is waiting on, released with it. */
  NuvashaTaskScope *operation;
} LoaderRequest;

void loader_request_free (LoaderRequest *request);
void on_java_for_the_install (GObject *source, GAsyncResult *result,
                              gpointer user_data);

#endif /* nuvasha_LAUNCHER_LOADER_PRIVATE_H */
