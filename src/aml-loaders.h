/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlLoaders: the mod loaders AML can install and run.
 *
 * A loader is a version document of its own that inherits from a
 * Minecraft version, so what a loader instance runs is that document
 * merged over the Minecraft one, which is what aml_version_description
 * _merge() already does for a profile that inherits from a parent.
 *
 * Two of them publish such a document themselves and nothing else:
 *
 *   Fabric Meta   meta.fabricmc.net/v2/versions/loader/<game>   the list
 *                 meta.fabricmc.net/v2/versions/loader/<game>/<loader>
 *                                                       /profile/json
 *
 *   Quilt Meta    meta.quiltmc.org/v3/versions/loader/<game>  the same
 *                 shape, and the profile at the same place.
 *
 * Both publish their libraries the way a maven repository does: a name
 * and the address of the repository, not a download. That is turned into
 * the downloads a version document carries, so the one resolve module
 * fetches everything from the library list of the merged document.
 *
 * The other two, NeoForge and Forge, publish an installer instead of a
 * document: see aml_loaders_install_async(), which is what they need.
 *
 * Nothing here starts a process or writes into an instance: this module
 * asks the loaders what they offer and hands back a document, and the
 * resolve module and the launcher do the rest. */

#ifndef AML_LOADERS_H
#define AML_LOADERS_H

#include <gio/gio.h>

#include "aml-instance.h"
#include "aml-minecraft-versions.h"

G_BEGIN_DECLS

/* Fabric Meta. */
#define AML_FABRIC_META_URL "https://meta.fabricmc.net/v2"

/* Quilt Meta. */
#define AML_QUILT_META_URL "https://meta.quiltmc.org/v3"

/* The maven repository Forge and NeoForge publish their installers in. */
#define AML_NEOFORGE_MAVEN_URL "https://maven.neoforged.net/releases"
#define AML_FORGE_MAVEN_URL "https://maven.minecraftforge.net"

#define AML_LOADERS_ERROR (aml_loaders_error_quark ())

typedef enum {
  AML_LOADERS_ERROR_UNSUPPORTED, /* AML does not install this loader */
  AML_LOADERS_ERROR_NO_VERSIONS, /* the loader has nothing for that game version */
  AML_LOADERS_ERROR_NO_MAVEN,    /* a library names no maven address */
  AML_LOADERS_ERROR_INSTALLER    /* the installer did not do its work */
} AmlLoadersError;

GQuark aml_loaders_error_quark (void);

/* One version of one loader. @stable is FALSE for a version the loader
 * itself does not call stable, which is what the "(unstable)" in the
 * lists is about:
 *
 *   Fabric says so: of the loaders that run a game version, the one the
 *   loader recommends is the only one with "stable" set.
 *   Quilt does not say anything, so a version that carries a pre-release
 *   suffix ("0.20.0-beta.9", "0.18.4-pre.3") is the one that is not
 *   stable. */
typedef struct {
  char *version;
  gboolean stable;
} AmlLoaderVersion;

void aml_loader_version_free (AmlLoaderVersion *version);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlLoaderVersion, aml_loader_version_free)

/* The loaders AML installs and runs a game with, in the order a user
 * reads them: Vanilla, Fabric, Quilt, Forge, NeoForge. A loader is in
 * that list only once it has been seen to start a game here. */
const AmlLoader *aml_loaders_supported (void);

/* How many loaders that list holds. Vanilla is one of them, so the list
 * has a length of its own rather than a sentinel. */
guint aml_loaders_supported_count (void);

/* TRUE when AML installs @loader and runs a game with it. */
gboolean aml_loader_is_supported (AmlLoader loader);

/* What a user reads: "Vanilla", "Fabric", "Quilt", "Forge" or
 * "NeoForge". Never NULL. */
const char *aml_loader_display_name (AmlLoader loader);

/* The versions of @loader that run @game_version, newest first, as the
 * loader publishes them. Nothing is filtered out here: what the user
 * asked to see is decided where the list is shown, so that a list that
 * is asked for again keeps what was asked for.
 *
 * AML_LOADERS_ERROR_UNSUPPORTED for a loader this module does not know,
 * AML_LOADERS_ERROR_NO_VERSIONS when the loader has no version for that
 * game version. */
void aml_loaders_versions_async (AmlLoader       loader,
                                 const char     *game_version,
                                 GCancellable   *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data);

/* (transfer full). The array holds AmlLoaderVersion* with
   aml_loader_version_free() as the element free function. */
GPtrArray *aml_loaders_versions_finish (GAsyncResult  *result,
                                        GError       **error);

/* The versions out of what a loader answered, newest first: the array of
 * {"loader": {"version": ..., "stable": ...}} entries Fabric Meta and
 * Quilt Meta publish for one Minecraft version. This is what the fetch
 * above parses, and what a test hands a fixture to.
 *
 * AML_LOADERS_ERROR_NO_VERSIONS when the answer holds no version at all.
 * (transfer full) */
GPtrArray *aml_loaders_versions_from_json (const char  *json,
                                           gssize       length,
                                           GError      **error);

/* The version document @loader_version of @loader for @game_version, in
 * the shape AML resolves: the libraries that publish a maven address
 * instead of a download get the download that address means, and nothing
 * else is touched. The document inherits from @game_version, so merging
 * it over the document of that version is all a loader instance needs.
 *
 * AML_LOADERS_ERROR_UNSUPPORTED for a loader this module does not know. */
void aml_loaders_profile_async (AmlLoader           loader,
                                const char         *game_version,
                                const char         *loader_version,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data);

/* (transfer full) */
AmlVersionDescription *aml_loaders_profile_finish (GAsyncResult  *result,
                                                  GError       **error);

/* The document a profile is, with the downloads its libraries mean: the
 * same work the fetch above does once the answer is parsed, and what a
 * test hands a fixture to. (transfer full) */
AmlVersionDescription *aml_loaders_profile_from_json (const char  *json,
                                                      gssize       length,
                                                      GError      **error);

G_END_DECLS

#endif /* AML_LOADERS_H */