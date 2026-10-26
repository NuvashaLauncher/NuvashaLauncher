/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_FS_H
#define nuvashalauncher_FS_H

#include <gio/gio.h>
#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

/* Bounded local-document policies. These prevent malformed/corrupt local
 * metadata from forcing unbounded parser allocations.
 *
 * The arithmetic is done in a 64-bit type on purpose: a limit written as
 * plain `1024 * 1024` is an `int` product, which silently overflows as soon
 * as a limit grows past 2 GiB instead of failing loudly. */
#define nuvashalauncher_FS_MAX_SMALL_METADATA_SIZE G_GINT64_CONSTANT (1024 * 1024)
#define nuvashalauncher_FS_MAX_CONTENT_INDEX_SIZE G_GINT64_CONSTANT (4 * 1024 * 1024)
#define nuvashalauncher_FS_MAX_INSTANCE_METADATA_SIZE G_GINT64_CONSTANT (16 * 1024 * 1024)
#define nuvashalauncher_FS_MAX_REMOTE_JSON_SIZE G_GINT64_CONSTANT (64 * 1024 * 1024)
#define nuvashalauncher_FS_MAX_ASSET_INDEX_SIZE nuvashalauncher_FS_MAX_REMOTE_JSON_SIZE
#define nuvashalauncher_FS_MAX_JSON_DOCUMENT_SIZE nuvashalauncher_FS_MAX_REMOTE_JSON_SIZE

gboolean nuvashalauncher_fs_replace_contents (const char *path, const void *contents,
                                  gssize length, int mode, GError **error);
gboolean nuvashalauncher_fs_make_private_directory (const char *path, GError **error);
gboolean nuvashalauncher_fs_make_directory (const char *path, int mode, GError **error);
gboolean nuvashalauncher_fs_get_contents_within (const char *path, goffset maximum,
                                      char **contents, gsize *length,
                                      GError **error);
gboolean nuvashalauncher_fs_unlink (const char *path, GError **error);
gboolean nuvashalauncher_fs_rename (const char *source, const char *destination,
                        GError **error);
int nuvashalauncher_fs_open_temporary_file (const char *directory, const char *prefix,
                                char **path, GError **error);
char *nuvashalauncher_fs_make_temporary_directory (const char *parent, const char *prefix,
                                       GError **error);
gboolean nuvashalauncher_fs_move_file (GFile *source, GFile *destination,
                           GFileCopyFlags flags, GCancellable *cancellable,
                           GError **error);
gboolean nuvashalauncher_fs_copy_file (GFile *source, GFile *destination,
                            GFileCopyFlags flags, GCancellable *cancellable,
                            GError **error);
void nuvashalauncher_fs_make_directory_async (GFile *file, int io_priority,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
gboolean nuvashalauncher_fs_make_directory_finish (GFile *file, GAsyncResult *result,
                                       GError **error);
void nuvashalauncher_fs_replace_contents_async (GFile *file, const char *contents,
                                    gsize length, const char *etag,
                                    gboolean make_backup, GFileCreateFlags flags,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);
gboolean nuvashalauncher_fs_replace_contents_finish (GFile *file, GAsyncResult *result,
                                          char **new_etag, GError **error);
void nuvashalauncher_fs_replace_contents_bytes_async (GFile *file, GBytes *contents,
                                          const char *etag, gboolean make_backup,
                                          GFileCreateFlags flags,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
gboolean nuvashalauncher_fs_replace_contents_bytes_finish (GFile *file, GAsyncResult *result,
                                               char **new_etag, GError **error);
void nuvashalauncher_fs_copy_async (GFile *source, GFile *destination, GFileCopyFlags flags,
                        int io_priority, GCancellable *cancellable,
                        GFileProgressCallback progress_callback,
                        gpointer progress_data, GAsyncReadyCallback callback,
                        gpointer user_data);
gboolean nuvashalauncher_fs_copy_finish (GFile *file, GAsyncResult *result,
                             GError **error);
void nuvashalauncher_fs_delete_async (GFile *file, int io_priority,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data);
gboolean nuvashalauncher_fs_delete_finish (GFile *file, GAsyncResult *result,
                               GError **error);

/* TRUE when every existing component of an absolute directory path can be
 * opened without following a symlink. Missing trailing components are
 * accepted so callers can create them securely afterwards. */
gboolean nuvashalauncher_fs_directory_is_real (const char *path);
int nuvashalauncher_fs_open_directory_nofollow (const char *path, GError **error);
int nuvashalauncher_fs_open_temporary_file_nofollow (const char *directory, char **name,
                                         char **path, int *parent_fd,
                                         GError **error);
gboolean nuvashalauncher_fs_unlink_at (int directory_fd, const char *name, GError **error);
gboolean nuvashalauncher_fs_rename_at (int directory_fd, const char *source,
                           const char *destination, GError **error);
gboolean nuvashalauncher_fs_make_directory_tree_nofollow (const char *directory,
                                              const char *relative, int mode,
                                              GError **error);
int nuvashalauncher_fs_open_file_nofollow (const char *directory, const char *relative,
                               int flags, int directory_mode, int mode,
                               GError **error);

/* Best-effort recursive removal that never follows symlinks. Used for
 * cleanup of private temporary trees; callers report the operation that
 * originally failed rather than replacing it with cleanup failure. */
void nuvashalauncher_fs_remove_tree (const char *path);

nuvashalauncherTaskScope *nuvashalauncher_fs_remove_tree_async (const char *path,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);
gboolean nuvashalauncher_fs_remove_tree_finish (GAsyncResult *result, GError **error);

nuvashalauncherTaskScope *nuvashalauncher_fs_copy_tree_async (GFile *source, GFile *destination,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);
gboolean nuvashalauncher_fs_copy_tree_finish (GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* nuvashalauncher_FS_H */
