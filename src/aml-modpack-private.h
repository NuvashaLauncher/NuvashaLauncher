/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_MODPACK_PRIVATE_H
#define AML_MODPACK_PRIVATE_H

#include "aml-modpack.h"

#define AML_MODPACK_FORMAT_VERSION 1

void extract_overrides_async (const char *archive_path,
                              const char *directory,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);
gboolean extract_overrides_finish (GAsyncResult *result, GError **error);

#endif /* AML_MODPACK_PRIVATE_H */
