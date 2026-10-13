/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_LOADERS_PRIVATE_H
#define AML_LOADERS_PRIVATE_H

#include "aml-loaders.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

const char *loader_meta (AmlLoader loader);
const char *loader_maven (AmlLoader loader, const char **coordinate);
gboolean give_libraries_their_downloads (JsonNode *root, GError **error);

char *aml_loader_jar_manifest_attribute (const char *path, const char *name);
gboolean aml_loader_write_jar_manifest_attribute (const char *path,
                                                  const char *name,
                                                  const char *value,
                                                  GError **error);
char *aml_loader_jar_main_class (const char *path, GError **error);
gboolean aml_loader_ensure_directory (const char *path, GError **error);
gboolean aml_loader_write_install_file (const char *path, const void *data,
                                        gsize size, GError **error);
GBytes *aml_loader_read_zip_entry (const char *path, const char *entry,
                                   GError **error);

G_END_DECLS

#endif /* AML_LOADERS_PRIVATE_H */
