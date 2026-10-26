/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherModpack: the Modrinth modpack format (.mrpack).
 *
 * A .mrpack is a ZIP containing modrinth.index.json at its root. The manifest
 * lists installed files and dependencies;
 * Everything the pack installs is in the manifest's files array; the
 * zip itself only carries the overrides, the extra files a pack lays
 * over the instance.
 *
 * Security is decided here, not by whoever walks the file list: paths
 * are checked, archives are bounded and downloads have to come from the
 * hosts the format names. */

#ifndef nuvashalauncher_MODPACK_H
#define nuvashalauncher_MODPACK_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-instance-manager.h"

G_BEGIN_DECLS

#define nuvashalauncher_MODPACK_ERROR (nuvashalauncher_modpack_error_quark ())

typedef enum {
  nuvashalauncher_MODPACK_ERROR_FORMAT,     /* the index is not what the format says */
  nuvashalauncher_MODPACK_ERROR_PATH,       /* a path would write outside the instance */
  nuvashalauncher_MODPACK_ERROR_ARCHIVE,    /* the .mrpack cannot be read as a zip */
  nuvashalauncher_MODPACK_ERROR_BOMB,       /* the archive is far larger than a pack */
  nuvashalauncher_MODPACK_ERROR_URL,        /* a download URL is not one nuvashalauncher accepts */
  nuvashalauncher_MODPACK_ERROR_HASH,       /* downloaded data failed verification */
  nuvashalauncher_MODPACK_ERROR_DOWNLOAD,   /* a file could not be downloaded */
  nuvashalauncher_MODPACK_ERROR_LOADER,     /* the Minecraft version or loader is unknown */
  nuvashalauncher_MODPACK_ERROR_IO,         /* the file system said no */
  nuvashalauncher_MODPACK_ERROR_CANCELLED   /* the caller cancelled the install */
} nuvashalauncherModpackError;

GQuark nuvashalauncher_modpack_error_quark (void);

/* Archive limits used to detect decompression bombs. A pack names
 * every file it installs in the manifest and the overrides are the
 * config files, resource packs and pre-generated worlds on top of them:
 * the biggest packs on Modrinth unpack well past a gigabyte, so the cap
 * is generous. It is the *actual* inflated bytes that are counted while
 * the overrides are extracted, not what an entry announces, so a small
 * archive that claims to expand without end is still stopped. */
#define nuvashalauncher_MODPACK_MAX_ENTRIES     (65536)
#define nuvashalauncher_MODPACK_MAX_CONTENT_SIZE (4ULL * 1024 * 1024 * 1024)

/* Client-side availability of a file. */
typedef enum {
  nuvashalauncher_MODPACK_ENV_UNSPECIFIED, /* no environment restriction */
  nuvashalauncher_MODPACK_ENV_REQUIRED,
  nuvashalauncher_MODPACK_ENV_OPTIONAL,
  nuvashalauncher_MODPACK_ENV_UNSUPPORTED
} nuvashalauncherModpackEnv;

/* One entry of the files array of the index. */
typedef struct
{
  char *path;              /* where in the instance it goes */
  char *sha1;
  char *sha512;
  goffset file_size;       /* the size the manifest announces, or 0 */
  nuvashalauncherModpackEnv env_client;
  nuvashalauncherModpackEnv env_server;
  GPtrArray *downloads;    /* char*, tried in the order given */
} nuvashalauncherModpackFile;

void nuvashalauncher_modpack_file_free (nuvashalauncherModpackFile *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherModpackFile, nuvashalauncher_modpack_file_free)

/* One entry in the dependency object: the required version for a name,
 * e.g. "minecraft" -> "1.20.1", "fabric-loader" -> "0.15.11". */
typedef struct
{
  char *name;
  char *version;
} nuvashalauncherModpackDependency;

void nuvashalauncher_modpack_dependency_free (nuvashalauncherModpackDependency *dependency);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherModpackDependency,
                               nuvashalauncher_modpack_dependency_free)

/* Parsed index data plus resolved dependency information. */
typedef struct
{
  guint format_version;
  char *game;              /* always "minecraft" to be one nuvashalauncher installs */
  char *version_id;
  char *name;
  char *summary;
  char *icon;               /* relative path inside the installed pack */
  GPtrArray *files;        /* nuvashalauncherModpackFile* */
  GPtrArray *dependencies; /* nuvashalauncherModpackDependency* */

  char *minecraft_version;
  nuvashalauncherLoader loader;
  char *loader_version;

  /* The .mrpack this index was read from, for extracting the
   * overrides at install time. Set by nuvashalauncher_modpack_new_from_zip(). */
  char *archive_path;
} nuvashalauncherModpack;

void nuvashalauncher_modpack_free (nuvashalauncherModpack *modpack);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherModpack, nuvashalauncher_modpack_free)

/* The manifest out of the JSON bytes. A path that leaves the instance,
 * a file with one hash missing or a format version other than the one
 * the format has today is a pack nuvashalauncher refuses to install. */
nuvashalauncherModpack *nuvashalauncher_modpack_new_from_json (const char *json,
                                       gssize length,
                                       GError **error);

/* The manifest out of the .mrpack on disk. Every entry of the archive
 * is checked before the manifest is even parsed: no symlink, no
 * hardlink, no path outside the instance, no bomb. */
nuvashalauncherModpack *nuvashalauncher_modpack_new_from_zip (const char *zip_path, GError **error);

nuvashalauncherTaskScope *nuvashalauncher_modpack_new_from_zip_async (const char *zip_path,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

nuvashalauncherModpack *nuvashalauncher_modpack_new_from_zip_finish (GAsyncResult *result,
                                             GError **error);

/* The Minecraft version and the loader the pack runs on, out of its
 * dependencies. A loader the pack does not name leaves the instance on
 * vanilla. */
gboolean nuvashalauncher_modpack_resolve (nuvashalauncherModpack *modpack, GError **error);

/* The files the pack requires to run on the client, and the ones it
 * only offers. (transfer full) one array per call, borrowing the
 * nuvashalauncherModpackFile* the pack owns, never NULL. */
GPtrArray *nuvashalauncher_modpack_required_client_files (nuvashalauncherModpack *modpack);
GPtrArray *nuvashalauncher_modpack_optional_client_files (nuvashalauncherModpack *modpack);

/* Both hashes of the manifest, out of the bytes on the disk. */
nuvashalauncherTaskScope *nuvashalauncher_modpack_verify_file_async (const nuvashalauncherModpackFile *file,
                                            const char *path,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean nuvashalauncher_modpack_verify_file_finish (GAsyncResult *result, GError **error);

/* The check every path the pack writes is run through. */
gboolean nuvashalauncher_modpack_path_is_safe (const char *path);

/* The cap on how much an archive may hold, in bytes. It defaults to
 * nuvashalauncher_MODPACK_MAX_CONTENT_SIZE; a test that cannot build a multi
 * gigabyte archive lowers it. The launcher never changes it. */
void nuvashalauncher_modpack_set_content_limit (guint64 bytes);
guint64 nuvashalauncher_modpack_get_content_limit (void);

/* The hosts the format permits the files to come from. */
gboolean nuvashalauncher_modpack_download_url_allowed (const char *url);

/* Progress reported between files during installation. */
typedef void (*nuvashalauncherModpackProgressFunc) (guint files_done,
                                        guint files_total,
                                        const char *path,
                                        gpointer user_data);

/* Downloads the files of @modpack, checks their hashes, lays the
 * overrides over them and only then moves everything next to
 * @directory and registers the instance with @manager.
 *
 * @directory may be NULL: the manager's own default for @name is
 * taken. @optional_files borrows nuvashalauncherModpackFile* out of @modpack for
 * the files the user picked, may be NULL.
 *
 * The install is built in a temporary directory next to the target.
 * Should anything fail, or the caller cancel, the directory is removed
 * and nothing touches an instance that was already there.
 *
 * Every download has to clear both hashes of the manifest. What the
 * user sees as "N of M files" goes to @progress. Both @callback and
 * @progress receive @user_data. */
nuvashalauncherTaskScope *nuvashalauncher_modpack_install_async (nuvashalauncherModpack *modpack,
                                         nuvashalauncherInstanceManager *manager,
                                         const char *name,
                                         const char *directory,
                                         const GPtrArray *optional_files,
                                         nuvashalauncherModpackProgressFunc progress,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/* (transfer full) the new instance, registered. NULL on error. */
nuvashalauncherInstance *nuvashalauncher_modpack_install_finish (GAsyncResult *result,
                                           GError **error);

gboolean nuvashalauncher_modpack_remove_archive (const char *path, GError **error);
gboolean nuvashalauncher_modpack_install_icon (nuvashalauncherInstanceManager *manager,
                                   nuvashalauncherInstance *instance,
                                   const char *source_path, GError **error);
gboolean nuvashalauncher_modpack_icon_is_regular (const char *path);

/* Writes @instance as a .mrpack at @path. The archive contains the
 * instance's manifest and its files under overrides/. Archive creation is
 * performed away from the GTK main loop. */
typedef void (*nuvashalauncherModpackExportProgressFunc) (guint64 done, guint64 total,
                                               const char *detail,
                                               gpointer user_data);

nuvashalauncherTaskScope *nuvashalauncher_modpack_export_async (nuvashalauncherInstance *instance, const char *path,
                                        const char *pack_name, const char *summary,
                                        const char *version_id,
                                        gboolean include_transient,
                                        nuvashalauncherModpackExportProgressFunc progress,
                                        gpointer progress_data,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

gboolean nuvashalauncher_modpack_export_finish (GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* nuvashalauncher_MODPACK_H */
