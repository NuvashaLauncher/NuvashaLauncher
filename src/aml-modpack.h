/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlModpack: the Modrinth modpack format (.mrpack).
 *
 * A .mrpack is a zip that holds modrinth.index.json at its root: the
 * manifest of the pack, what it comes with and what it needs around it.
 * Everything the pack installs is in the manifest's files array; the
 * zip itself only carries the overrides, the extra files a pack lays
 * over the instance.
 *
 * Security is decided here, not by whoever walks the file list: paths
 * are checked, archives are bounded and downloads have to come from the
 * hosts the format names. */

#ifndef AML_MODPACK_H
#define AML_MODPACK_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "aml-download.h"
#include "aml-instance-manager.h"

G_BEGIN_DECLS

#define AML_MODPACK_ERROR (aml_modpack_error_quark ())

typedef enum {
  AML_MODPACK_ERROR_FORMAT,     /* the index is not what the format says */
  AML_MODPACK_ERROR_PATH,       /* a path would write outside the instance */
  AML_MODPACK_ERROR_ARCHIVE,    /* the .mrpack cannot be read as a zip */
  AML_MODPACK_ERROR_BOMB,       /* the archive is far larger than a pack */
  AML_MODPACK_ERROR_URL,        /* a download URL is not one AML accepts */
  AML_MODPACK_ERROR_HASH,       /* what arrived does not match the index */
  AML_MODPACK_ERROR_DOWNLOAD,   /* a file could not be downloaded */
  AML_MODPACK_ERROR_LOADER,     /* the Minecraft version or loader is unknown */
  AML_MODPACK_ERROR_IO,         /* the file system said no */
  AML_MODPACK_ERROR_CANCELLED   /* the caller cancelled the install */
} AmlModpackError;

GQuark aml_modpack_error_quark (void);

/* What the archive may hold before it is called a bomb. A pack names
 * every file it installs in the manifest and the overrides are the
 * config files, resource packs and pre-generated worlds on top of them:
 * the biggest packs on Modrinth unpack well past a gigabyte, so the cap
 * is generous. It is the *actual* inflated bytes that are counted while
 * the overrides are extracted, not what an entry announces, so a small
 * archive that claims to expand without end is still stopped. */
#define AML_MODPACK_MAX_ENTRIES     (65536)
#define AML_MODPACK_MAX_CONTENT_SIZE (4ULL * 1024 * 1024 * 1024)

/* The three states of a file for the client: the pack requires it, it
 * only offers it, or the client must not see it. */
typedef enum {
  AML_MODPACK_ENV_UNSPECIFIED, /* no env object: what both sides want */
  AML_MODPACK_ENV_REQUIRED,
  AML_MODPACK_ENV_OPTIONAL,
  AML_MODPACK_ENV_UNSUPPORTED
} AmlModpackEnv;

/* One entry of the files array of the index. */
typedef struct
{
  char *path;              /* where in the instance it goes */
  char *sha1;
  char *sha512;
  goffset file_size;       /* the size the manifest announces, or 0 */
  AmlModpackEnv env_client;
  AmlModpackEnv env_server;
  GPtrArray *downloads;    /* char*, tried in the order given */
} AmlModpackFile;

void aml_modpack_file_free (AmlModpackFile *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlModpackFile, aml_modpack_file_free)

/* One entry of the dependencies object: what the pack needs by name,
 * e.g. "minecraft" -> "1.20.1", "fabric-loader" -> "0.15.11". */
typedef struct
{
  char *name;
  char *version;
} AmlModpackDependency;

void aml_modpack_dependency_free (AmlModpackDependency *dependency);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlModpackDependency,
                               aml_modpack_dependency_free)

/* What the index holds, plus what the dependencies say once
 * aml_modpack_resolve() read them. */
typedef struct
{
  guint format_version;
  char *game;              /* always "minecraft" to be one AML installs */
  char *version_id;
  char *name;
  char *summary;
  char *icon;               /* relative path inside the installed pack */
  GPtrArray *files;        /* AmlModpackFile* */
  GPtrArray *dependencies; /* AmlModpackDependency* */

  char *minecraft_version;
  AmlLoader loader;
  char *loader_version;

  /* The .mrpack this index was read from, for extracting the
   * overrides at install time. Set by aml_modpack_new_from_zip(). */
  char *archive_path;
} AmlModpack;

void aml_modpack_free (AmlModpack *modpack);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlModpack, aml_modpack_free)

/* The manifest out of the JSON bytes. A path that leaves the instance,
 * a file with one hash missing or a format version other than the one
 * the format has today is a pack AML refuses to install. */
AmlModpack *aml_modpack_new_from_json (const char *json,
                                       gssize length,
                                       GError **error);

/* The manifest out of the .mrpack on disk. Every entry of the archive
 * is checked before the manifest is even parsed: no symlink, no
 * hardlink, no path outside the instance, no bomb. */
AmlModpack *aml_modpack_new_from_zip (const char *zip_path, GError **error);

AmlTaskScope *aml_modpack_new_from_zip_async (const char *zip_path,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

AmlModpack *aml_modpack_new_from_zip_finish (GAsyncResult *result,
                                             GError **error);

/* The Minecraft version and the loader the pack runs on, out of its
 * dependencies. A loader the pack does not name leaves the instance on
 * vanilla. */
gboolean aml_modpack_resolve (AmlModpack *modpack, GError **error);

/* The files the pack requires to run on the client, and the ones it
 * only offers. (transfer full) one array per call, borrowing the
 * AmlModpackFile* the pack owns, never NULL. */
GPtrArray *aml_modpack_required_client_files (AmlModpack *modpack);
GPtrArray *aml_modpack_optional_client_files (AmlModpack *modpack);

/* Both hashes of the manifest, out of the bytes on the disk. */
AmlTaskScope *aml_modpack_verify_file_async (const AmlModpackFile *file,
                                            const char *path,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean aml_modpack_verify_file_finish (GAsyncResult *result, GError **error);

/* The check every path the pack writes is run through. */
gboolean aml_modpack_path_is_safe (const char *path);

/* The cap on how much an archive may hold, in bytes. It defaults to
 * AML_MODPACK_MAX_CONTENT_SIZE; a test that cannot build a multi
 * gigabyte archive lowers it. The launcher never changes it. */
void aml_modpack_set_content_limit (guint64 bytes);
guint64 aml_modpack_get_content_limit (void);

/* The hosts the format permits the files to come from. */
gboolean aml_modpack_download_url_allowed (const char *url);

/* What an install reports between files. */
typedef void (*AmlModpackProgressFunc) (guint files_done,
                                        guint files_total,
                                        const char *path,
                                        gpointer user_data);

/* Downloads the files of @modpack, checks their hashes, lays the
 * overrides over them and only then moves everything next to
 * @directory and registers the instance with @manager.
 *
 * @directory may be NULL: the manager's own default for @name is
 * taken. @optional_files borrows AmlModpackFile* out of @modpack for
 * the files the user picked, may be NULL.
 *
 * The install is built in a temporary directory next to the target.
 * Should anything fail, or the caller cancel, the directory is removed
 * and nothing touches an instance that was already there.
 *
 * Every download has to clear both hashes of the manifest. What the
 * user sees as "N of M files" goes to @progress. Both @callback and
 * @progress receive @user_data. */
AmlTaskScope *aml_modpack_install_async (AmlModpack *modpack,
                                         AmlInstanceManager *manager,
                                         const char *name,
                                         const char *directory,
                                         const GPtrArray *optional_files,
                                         AmlModpackProgressFunc progress,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/* (transfer full) the new instance, registered. NULL on error. */
AmlInstance *aml_modpack_install_finish (GAsyncResult *result,
                                           GError **error);

gboolean aml_modpack_remove_archive (const char *path, GError **error);
gboolean aml_modpack_install_icon (AmlInstanceManager *manager,
                                   AmlInstance *instance,
                                   const char *source_path, GError **error);
gboolean aml_modpack_icon_is_regular (const char *path);
void aml_modpack_operation_release (AmlTaskScope *operation,
                                    GCancellable *cancellable);
AmlTaskScope *aml_modpack_operation_new (GCancellable *cancellable);
void aml_modpack_operation_cancel (AmlTaskScope *operation);
void aml_modpack_operation_forget (AmlTaskScope *operation);
void aml_modpack_operation_follow (AmlTaskScope *operation,
                                   GCancellable *cancellable);
void aml_modpack_operation_track (AmlTaskScope *operation, guint source_id);
void aml_modpack_operation_remove (AmlTaskScope *operation, guint source_id);
void aml_modpack_operation_forget_source (AmlTaskScope *operation,
                                          guint source_id);

/* Writes @instance as a .mrpack at @path. The archive contains the
 * instance's manifest and its files under overrides/. Archive creation is
 * performed away from the GTK main loop. */
typedef void (*AmlModpackExportProgressFunc) (guint64 done, guint64 total,
                                               const char *detail,
                                               gpointer user_data);

AmlTaskScope *aml_modpack_export_async (AmlInstance *instance, const char *path,
                                        const char *pack_name, const char *summary,
                                        const char *version_id,
                                        gboolean include_transient,
                                        AmlModpackExportProgressFunc progress,
                                        gpointer progress_data,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

gboolean aml_modpack_export_finish (GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* AML_MODPACK_H */
