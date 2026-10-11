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

/* TRUE when every existing component of an absolute directory path can be
 * opened without following a symlink. Missing trailing components are
 * accepted so callers can create them securely afterwards. */
gboolean aml_fs_directory_is_real (const char *path);
int aml_fs_open_directory_nofollow (const char *path, GError **error);

/* Best-effort recursive removal that never follows symlinks. Used for
 * cleanup of private temporary trees; callers report the operation that
 * originally failed rather than replacing it with cleanup failure. */
void aml_fs_remove_tree (const char *path);

void aml_fs_remove_tree_async (const char *path, GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean aml_fs_remove_tree_finish (GAsyncResult *result, GError **error);

void aml_fs_copy_tree_async (GFile *source, GFile *destination,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data);
gboolean aml_fs_copy_tree_finish (GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* AML_FS_H */
