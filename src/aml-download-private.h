/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_DOWNLOAD_PRIVATE_H
#define AML_DOWNLOAD_PRIVATE_H

#include "aml-download.h"

/* Shared only by the transfer scheduler and the local-file hashing unit. */
AmlTaskScope *aml_download_hash_file_async (GFile *file,
                                            GChecksumType type,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);
char *aml_download_hash_file_finish (GAsyncResult *result, GError **error);

#endif
