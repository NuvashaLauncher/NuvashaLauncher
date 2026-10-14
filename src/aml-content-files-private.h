/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_CONTENT_FILES_PRIVATE_H
#define AML_CONTENT_FILES_PRIVATE_H

#include "aml-content-page.h"

void aml_content_apply_local_metadata (AmlContentFile *file,
                                       AmlContentType type);
gboolean aml_content_load_local_icon (AmlContentFile *file);
AmlTaskScope *aml_content_load_local_icon_async (AmlContentFile *file,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
char *aml_content_load_local_icon_finish (GAsyncResult *result,
                                          GError **error);
void aml_content_merge_dependencies (GPtrArray **destination,
                                     GPtrArray *source);

#endif
