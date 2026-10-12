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
 * document. Their installer is a jar that carries an install profile: the
 * version document to merge over the Minecraft one, and a list of
 * processors that patch the client jar. This module runs those
 * processors with a Java, which is what aml_loaders_install_async()
 * does, and hands back what the game then runs.
 *
 * Which versions of those two AML installs is what their install profiles
 * allow. The current ones it follows whole: every processor of the client
 * side, with the artifacts it needs fetched out of the maven the loader
 * publishes, which is what NeoForge 21 and newer and Forge 1.20.5 and
 * newer ask for. The generations before those fetch their mappings from
 * Mojang's own maven instead, which is not a repository this module knows
 * about, so their profiles come out of it half-fulfilled: the install
 * stops there, with what the processor that failed said about it, rather
 * than producing a game that cannot start.
 *
 * Nothing here writes into an instance: a loader is installed into its own
 * directory under the shared data directory, and the launcher runs the
 * game from the instance. */

#ifndef AML_LOADERS_H
#define AML_LOADERS_H

#include <gio/gio.h>

#include "aml-instance.h"
#include "aml-minecraft-versions.h"
#include "aml-task.h"

G_BEGIN_DECLS

/* Fabric Meta. */
#define AML_FABRIC_META_URL "https://meta.fabricmc.net/v2"

/* Quilt Meta. */
#define AML_QUILT_META_URL "https://meta.quiltmc.org/v3"

/* The maven repository NeoForge and Forge publish their installers in,
 * and where the artifacts of a profile are fetched from. */
#define AML_NEOFORGE_MAVEN_URL "https://maven.neoforged.net/releases"
#define AML_FORGE_MAVEN_URL "https://maven.minecraftforge.net"

/* The host the version list API of those two hangs off. NeoForge keeps
 * its repository under /releases but its API on the host itself, so the
 * two are not the same base. */
#define AML_NEOFORGE_API_URL "https://maven.neoforged.net"
#define AML_FORGE_API_URL "https://maven.minecraftforge.net"

/* Where a loader is installed, under the same data directory the resolve
 * module writes the games it fetches into. */
#define AML_LOADERS_SUBDIR "loaders"

#define AML_LOADERS_ERROR (aml_loaders_error_quark ())

typedef enum {
  AML_LOADERS_ERROR_UNSUPPORTED, /* AML does not install this loader */
  AML_LOADERS_ERROR_NO_VERSIONS, /* the loader has nothing for that game version */
  AML_LOADERS_ERROR_NO_MAVEN,    /* a library names no maven address */
  AML_LOADERS_ERROR_INSTALLER,   /* the installer could not be read or run */
  AML_LOADERS_ERROR_GENERATION   /* the version is of a generation whose
                                  * install profile is not run here */
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

/* TRUE when a loader is installed by running the installer it publishes
 * rather than by fetching a version document of its own: NeoForge and
 * Forge do, Fabric and Quilt do not. This is what tells the launcher
 * which of the two ways of running a loader an instance needs. */
gboolean aml_loader_publishes_an_installer (AmlLoader loader);

/* TRUE when @loader_version of @loader is a version that runs
 * @game_version. Both of the loaders that publish an installer name
 * their versions after the Minecraft version they run, so the question is
 * answered by the name: Forge puts a dash between the two
 * ("1.20.1-47.2.0") and NeoForge a dot ("26.3.0.51-beta"). */
gboolean aml_loaders_version_runs_game (AmlLoader      loader,
                                        const char    *loader_version,
                                        const char    *game_version);

/* The versions of @loader that run @game_version, newest first, as the
 * loader publishes them. Nothing is filtered out here: what the user
 * asked to see is decided where the list is shown, so that a list that
 * is asked for again keeps what was asked for.
 *
 * AML_LOADERS_ERROR_UNSUPPORTED for a loader this module does not know,
 * AML_LOADERS_ERROR_NO_VERSIONS when the loader has no version for that
 * game version. */
AmlTaskScope *aml_loaders_versions_async (AmlLoader loader,
                                          const char *game_version,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

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

/* The versions out of what the maven of a loader that publishes an
 * installer answered, newest first: everything it holds, as
 * {"versions": ["..."]}, of which only the ones named after
 * @game_version are versions of it. What the loader published after the
 * Minecraft version in a name is read for a pre-release, so that
 * "26.3.0.51-beta" is not stable and "1.20.1-47.2.0" is, even though
 * both names carry a dash.
 *
 * AML_LOADERS_ERROR_NO_VERSIONS when the answer holds no version of that
 * game. (transfer full) */
GPtrArray *aml_loaders_maven_versions_from_json (AmlLoader      loader,
                                                 const char    *game_version,
                                                 const char    *json,
                                                 gssize         length,
                                                 GError       **error);

/* What a version list shows of what a loader published: the versions
 * @show_unstable keeps added to the ones the loader calls stable, and
 * @wanted kept even when it would otherwise be hidden, because an
 * instance that runs a version must never lose it just because it is old
 * or unstable. Newest first.
 *
 * @selected is set to the position of @wanted when it is in the list,
 * otherwise to the position of the newest stable version, and to
 * G_MAXUINT when there is nothing to pick. (transfer full) */
GPtrArray *aml_loaders_versions_for_display (GPtrArray  *published,
                                             gboolean    show_unstable,
                                             const char *wanted,
                                             guint      *selected);

/* The version document @loader_version of @loader for @game_version, in
 * the shape AML resolves: the libraries that publish a maven address
 * instead of a download get the download that address means, and nothing
 * else is touched. The document inherits from @game_version, so merging
 * it over the document of that version is all a loader instance needs.
 *
 * AML_LOADERS_ERROR_UNSUPPORTED for a loader this module does not know. */
AmlTaskScope *aml_loaders_profile_async (AmlLoader loader,
                                         const char *game_version,
                                         const char *loader_version,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/* (transfer full) */
AmlVersionDescription *aml_loaders_profile_finish (GAsyncResult  *result,
                                                  GError       **error);

/* ---------------------------------------------------------- *
 * The loaders that publish an installer
 * ---------------------------------------------------------- */

/* What an installer leaves behind for the game: the version document to
 * merge over the Minecraft one, and the jar the game starts from. */
typedef struct {
  AmlVersionDescription *description; /* transfer full, never NULL */
  char *client_jar;                   /* transfer full, never NULL */
} AmlLoaderInstall;

void aml_loader_install_free (AmlLoaderInstall *install);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlLoaderInstall, aml_loader_install_free)

/* What an install is doing, for a caller that shows it: @stage is a short
 * line of words, @done of @total files is how far along it is. Called on
 * the caller's main context. */
typedef void (*AmlLoadersProgressFunc) (const char *stage,
                                        guint       done,
                                        guint       total,
                                        gpointer    user_data);

/* Installs @loader_version of @loader and returns what the instance then
 * runs.
 *
 * @game_document is the document of @game_version, the Minecraft version
 * the loader runs: it is where the client jar the installer patches comes
 * from, and the document that comes back is merged over it by the caller,
 * the same merge a profile inherits from a parent.
 *
 * @java_executable runs the processors, so it is the Java the game runs
 * with afterwards. @data_dir is where the loader is installed, under the
 * "loaders" directory of it; NULL means the shared data directory, the
 * one the resolve module fetches games into.
 *
 * Nothing is installed twice: what a previous install left is used as it
 * is when it is all there.
 *
 * The jar the processors leave behind is also told which distribution it
 * is, in the manifest of the jar, because the loader of the game reads
 * that one line before it loads anything and stops with "the installation
 * is corrupted" when the jar says nothing. A jar that already says it is
 * left as it is.
 *
 * AML_LOADERS_ERROR_INSTALLER when the installer cannot be read or one of
 * its processors fails, in which case the error carries what the
 * processor printed, and AML_LOADERS_ERROR_GENERATION for a version whose
 * install profile is of a shape this does not run. Either way the install
 * stops with what went wrong in words rather than going on to a game that
 * would not start.
 *
 * AML_LOADERS_ERROR_UNSUPPORTED for a loader that publishes no installer. */
AmlTaskScope *aml_loaders_install_async (AmlLoader loader,
                                         const char *game_version,
                                         const char *loader_version,
                                         AmlVersionDescription *game_document,
                                         const char *java_executable,
                                         const char *data_dir,
                                         AmlLoadersProgressFunc progress,
                                         gpointer progress_data,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/* (transfer full). NULL on error. */
AmlLoaderInstall *aml_loaders_install_finish (GAsyncResult  *result,
                                              GError       **error);

/* The document a profile is, with the downloads its libraries mean: the
 * same work the fetch above does once the answer is parsed, and what a
 * test hands a fixture to. (transfer full) */
AmlVersionDescription *aml_loaders_profile_from_json (const char  *json,
                                                      gssize       length,
                                                      GError      **error);

G_END_DECLS

#endif /* AML_LOADERS_H */