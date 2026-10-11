/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_FS_H
#define AML_FS_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Bounded local-document policies. These prevent malformed/corrupt local
 * metadata from forcing unbounded parser allocations.
 *
 * The arithmetic is done in a 64-bit type on purpose: a limit written as
 * plain `1024 * 1024` is an `int` product, which silently overflows as soon
 * as a limit grows past 2 GiB instead of failing loudly. */
#define AML_FS_MAX_SMALL_METADATA_SIZE G_GINT64_CONSTANT (1024 * 1024)
#define AML_FS_MAX_CONTENT_INDEX_SIZE G_GINT64_CONSTANT (4 * 1024 * 1024)
#define AML_FS_MAX_INSTANCE_METADATA_SIZE G_GINT64_CONSTANT (16 * 1024 * 1024)
#define AML_FS_MAX_REMOTE_JSON_SIZE G_GINT64_CONSTANT (64 * 1024 * 1024)
#define AML_FS_MAX_ASSET_INDEX_SIZE AML_FS_MAX_REMOTE_JSON_SIZE
#define AML_FS_MAX_JSON_DOCUMENT_SIZE AML_FS_MAX_REMOTE_JSON_SIZE

gboolean aml_fs_replace_contents (const char *path, const void *contents,
                                  gssize length, int mode, GError **error);
gboolean aml_fs_make_private_directory (const char *path, GError **error);
gboolean aml_fs_make_directory (const char *path, int mode, GError **error);
gboolean aml_fs_file_size_within (const char *path, goffset maximum,
                                  GError **error);
gboolean aml_fs_unlink (const char *path, GError **error);
gboolean aml_fs_rename (const char *source, const char *destination,
                        GError **error);
int aml_fs_open_temporary_file (const char *directory, const char *prefix,
                                char **path, GError **error);
char *aml_fs_make_temporary_directory (const char *parent, const char *prefix,
                                       GError **error);
gboolean aml_fs_move_file (GFile *source, GFile *destination,
                           GFileCopyFlags flags, GCancellable *cancellable,
                           GError **error);
gboolean aml_fs_copy_file (GFile *source, GFile *destination,
                           GFileCopyFlags flags, GCancellable *cancellable,
                           GError **error);
void aml_fs_replace_contents_async (GFile *file, const char *contents,
                                    gsize length, const char *etag,
                                    gboolean make_backup, GFileCreateFlags flags,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);
gboolean aml_fs_replace_contents_finish (GFile *file, GAsyncResult *result,
                                         char **new_etag, GError **error);
void aml_fs_copy_async (GFile *source, GFile *destination, GFileCopyFlags flags,
                        int io_priority, GCancellable *cancellable,
                        GFileProgressCallback progress_callback,
                        gpointer progress_data, GAsyncReadyCallback callback,
                        gpointer user_data);
gboolean aml_fs_copy_finish (GFile *file, GAsyncResult *result,
                             GError **error);
void aml_fs_delete_async (GFile *file, int io_priority,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data);
gboolean aml_fs_delete_finish (GFile *file, GAsyncResult *result,
                               GError **error);

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
