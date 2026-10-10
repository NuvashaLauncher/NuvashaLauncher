/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_FS_H
#define AML_FS_H

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean aml_fs_replace_contents (const char *path, const void *contents,
                                  gssize length, int mode, GError **error);
gboolean aml_fs_make_private_directory (const char *path, GError **error);

G_END_DECLS

#endif /* AML_FS_H */
