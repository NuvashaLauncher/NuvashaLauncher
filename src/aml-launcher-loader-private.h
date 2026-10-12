/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_LAUNCHER_LOADER_PRIVATE_H
#define AML_LAUNCHER_LOADER_PRIVATE_H

#include "aml-launcher-private.h"

typedef struct {
  AmlLauncher *launcher; /* the reference this work holds */
  AmlVersionManifest *manifest;
  AmlLoader loader;
  AmlVersionDescription *profile; /* the document the loader adds */
  char *game_version;
  char *loader_version;

  /* The operation this request is waiting on, released with it. */
  AmlTaskScope *operation;
} LoaderRequest;

void loader_request_free (LoaderRequest *request);
void on_java_for_the_install (GObject *source, GAsyncResult *result,
                              gpointer user_data);

#endif /* AML_LAUNCHER_LOADER_PRIVATE_H */
