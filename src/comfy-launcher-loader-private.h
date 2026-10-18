/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_LAUNCHER_LOADER_PRIVATE_H
#define comfy_LAUNCHER_LOADER_PRIVATE_H

#include "comfy-launcher-private.h"

typedef struct {
  ComfyLauncher *launcher; /* the reference this work holds */
  ComfyVersionManifest *manifest;
  ComfyLoader loader;
  ComfyVersionDescription *profile; /* the document the loader adds */
  char *game_version;
  char *loader_version;

  /* The operation this request is waiting on, released with it. */
  ComfyTaskScope *operation;
} LoaderRequest;

void loader_request_free (LoaderRequest *request);
void on_java_for_the_install (GObject *source, GAsyncResult *result,
                              gpointer user_data);

#endif /* comfy_LAUNCHER_LOADER_PRIVATE_H */
