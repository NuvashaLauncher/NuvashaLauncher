/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaResolve: getting a Minecraft version onto this disk, without
 * starting it.
 *
 * A version document names a client jar, a set of libraries (some of
 * them carrying natives for Linux) and an asset index. This module works
 * out where each of those belongs below g_get_user_data_dir()/nuvasha,
 * fetches what is missing with the download module (which verifies the
 * sha1 the document published and caps how many transfers run at once),
 * unpacks the natives into a directory java.library.path can be pointed
 * at, and hands back the classpath, the arguments and the few values the
 * launching code needs. Nothing here starts a process.
 *
 * The work runs asynchronously in the caller's thread-default main
 * context, normally the GTK main loop: nothing here waits for a socket,
 * for a disk or for a jar in that loop. The path, classifier, ordering
 * and argument helpers are plain functions over data that is already in
 * memory; the tests use them on their own without a network. */

#ifndef nuvasha_MINECRAFT_RESOLVE_H
#define nuvasha_MINECRAFT_RESOLVE_H

#include <gio/gio.h>

#include "nuvasha-minecraft-versions.h"
#include "nuvasha-task.h"

G_BEGIN_DECLS

/* Directory below g_get_user_data_dir() holding the client jars, the
 * libraries, the assets, the natives and the logging configurations. */
#define nuvasha_RESOLVE_DATA_SUBDIR "nuvasha"

/* Where Mojang serves the objects of an asset index: the hash of the
 * object, split after two characters. */
#define nuvasha_RESOLVE_ASSET_BASE_URL "https://resources.download.minecraft.net"

#define nuvasha_RESOLVE_ERROR (nuvasha_resolve_error_quark ())

typedef enum {
  nuvasha_RESOLVE_ERROR_IO,          /* a directory or file could not be made */
  nuvasha_RESOLVE_ERROR_INVALID,      /* the version cannot be resolved as it is */
  nuvasha_RESOLVE_ERROR_UNSAFE_PATH,  /* a jar entry points out of its directory */
  nuvasha_RESOLVE_ERROR_ASSETS        /* the asset index is not what we expect */
} NuvashaResolveError;

GQuark nuvasha_resolve_error_quark (void);

/* ---------------------------------------------------------- *
 * Maven paths
 * ---------------------------------------------------------- */

/* The maven path of a coordinate written as
 * "group:artifact:version[:classifier][@extension]": the dots of the
 * group become directories and the file is
 * "artifact-version[-classifier].extension". "org.lwjgl:lwjgl:3.3.3"
 * therefore gives "org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar", and
 * "de.oceanlabs.mcp:mcp_config:1.21-20240808.103037@zip" gives
 * "de/oceanlabs/mcp/mcp_config/1.21-20240808.103037/mcp_config-1.21-20240808.103037.zip".
 * An absent extension means jar. NULL when the string is not a
 * coordinate. */
char *nuvasha_resolve_maven_path (const char *coordinate);

/* The same path with @classifier taken from elsewhere, which is how the
 * natives of a library are named when the coordinate itself does not
 * say so. NULL when either part cannot be used. */
char *nuvasha_resolve_maven_path_classified (const char *coordinate,
                                         const char *classifier);

/* ---------------------------------------------------------- *
 * Choosing what a version needs on this machine
 * ---------------------------------------------------------- */

/* The classifier of the natives jar that fits @context, or NULL when
 * the library has none.
 *
 * The "natives" table of the library is asked first ("linux" ->
 * "natives-linux"). Libraries that publish one natives jar per machine
 * are then looked for under the architecture of @context:
 * "natives-linux-x86_64", "natives-linux-amd64", "natives-linux-arm64",
 * each tried in turn for that architecture. */
const char *nuvasha_resolve_native_classifier (const NuvashaLibrary       *library,
                                           const NuvashaRuleContext *context);

/* TRUE when @entry_name may be written below the directory it came
 * from: not absolute, no "..", no "." component, no empty component, no
 * backslash. A jar is data from the network, so nothing it holds is
 * taken on trust. */
gboolean nuvasha_resolve_entry_name_is_safe (const char *entry_name);

/* ---------------------------------------------------------- *
 * What has to be on disk
 * ---------------------------------------------------------- */

/* One file a version needs. @path is where it belongs, @url is where it
 * comes from and @sha1 is what it must hash to; @sha1 is NULL when the
 * document published none, in which case nothing can be verified. A
 * natives file also carries the entry prefixes its jar must not be
 * unpacked for, borrowed from the version document and NULL when the
 * document lists none. */
typedef struct {
  char *path;
  char *url;
  char *sha1;
  gboolean natives;
  GPtrArray *exclude;
} NuvashaResolveFile;

void nuvasha_resolve_file_free (NuvashaResolveFile *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaResolveFile, nuvasha_resolve_file_free)

/* Every jar this version needs on this machine, in the order of the
 * document: the artifact of each applicable library, followed by its
 * natives jar when it publishes one the old way.
 *
 * A library whose coordinate carries a classifier that starts with
 * "natives", the way Mojang has written them since 1.20.5, is an
 * ordinary jar as far as this module is concerned: it is fetched and it
 * goes on the classpath, because that is where LWJGL, JNA and netty
 * find the natives they unpack themselves. Only the "natives" table of
 * an older document means a jar to unpack. (transfer full)
 *
 * @libraries_dir is the directory the jars belong below. Each path is
 * the published path of the jar when it has one, and the maven path of
 * the coordinate otherwise. A library that is applicable but that names
 * no jar to fetch is left out. Nothing is created here: the paths are
 * what they are, and the caller makes the directories the download
 * module needs. */
GPtrArray *nuvasha_resolve_library_files (NuvashaVersionDescription *description,
                                      const NuvashaRuleContext   *context,
                                      const char             *libraries_dir);

/* The classpath of this version on this machine: every applicable
 * library jar in the order of the document, then @client_jar, joined
 * with the separator java expects for -cp and CLASSPATH_PATH_SEPARATOR.
 * (transfer full) */
char *nuvasha_resolve_classpath (NuvashaVersionDescription *description,
                             const NuvashaRuleContext   *context,
                             const char             *libraries_dir,
                             const char             *client_jar);

/* ---------------------------------------------------------- *
 * Arguments
 * ---------------------------------------------------------- */

/* The jvm arguments of the document that apply here, as a flat array of
 * strings holding the values of every allowed entry in order, with the
 * placeholders the document wrote left in. (transfer full) */
GPtrArray *nuvasha_resolve_jvm_arguments (NuvashaVersionDescription *description,
                                      const NuvashaRuleContext   *context);

/* The game arguments of the document that apply here, as a flat array
 * of strings. A document without the modern "arguments" object has one
 * line instead, which is split the way a shell would. (transfer full) */
GPtrArray *nuvasha_resolve_game_arguments (NuvashaVersionDescription *description,
                                       const NuvashaRuleContext   *context);

/* ---------------------------------------------------------- *
 * Unpacking the natives
 * ---------------------------------------------------------- */

/* Unpacks every jar of @jars (NuvashaResolveFile*) into @natives_directory,
 * honouring the exclude list of each one, which is where "META-INF/"
 * ends up.
 *
 * Only regular files and directories are written. A symlink, a hard
 * link or any other kind of entry inside a jar is skipped, so a jar
 * cannot make a name of the natives directory point somewhere else on
 * the disk, and every directory on the way has to be a real one. An
 * entry name that is not safe fails the whole jar with
 * nuvasha_RESOLVE_ERROR_UNSAFE_PATH instead of being written.
 *
 * A jar that lists no prefixes of its own is taken to exclude META-INF,
 * which is what every launcher does with the metadata a jar carries. */
NuvashaTaskScope *nuvasha_resolve_extract_natives_async (GPtrArray *jars,
                                                 const char *natives_directory,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data);

gboolean nuvasha_resolve_extract_natives_finish (GAsyncResult  *result,
                                             GError       **error);

/* ---------------------------------------------------------- *
 * A version that is ready
 * ---------------------------------------------------------- */

typedef struct {
  char *version_id;
  char *version_type;       /* release, snapshot, old_beta or old_alpha */
  char *client_jar;         /* absolute path */
  char *classpath;          /* ':' separated, client jar last */
  char *natives_directory;
  char *assets_directory;   /* the assets root to hand the game */
  char *assets_index_name;  /* NULL when the index is virtual */
  char *main_class;
  char *library_directory;
  char *logging_argument;   /* NULL when the version has no logging file */
  GPtrArray *jvm_arguments;  /* char*, known placeholders substituted */
  GPtrArray *game_arguments; /* char*, the rest left in */
  int java_major_version;   /* 0 when the document names no Java version */
} NuvashaResolvedVersion;

void nuvasha_resolved_version_free (NuvashaResolvedVersion *resolved);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaResolvedVersion, nuvasha_resolved_version_free)

/* What the resolve is doing and how much of it is done. @stage is
 * "dependencies" while the jars, the client and the logging file are
 * fetched and "assets" while the index and its objects are;
 * @done counts the files that are finished and @total is how many the
 * current stage has. */
typedef struct {
  const char *stage;
  guint done;
  guint total;
} NuvashaResolveProgress;

typedef void (*NuvashaResolveProgressFunc) (const NuvashaResolveProgress *progress,
                                        gpointer                 user_data);

/* Fetches @version_id from @manifest and puts everything it needs on
 * disk: the client jar, the libraries, the natives unpacked into
 * @natives_dir, the asset index with every object, and the logging
 * configuration. A file that is already there with the published sha1 is
 * used as it is, so a second run asks the network for nothing.
 *
 * @data_dir is where the versions, libraries, assets and logging files
 * live; NULL means g_get_user_data_dir()/nuvasha. @natives_dir is where the
 * natives are unpacked; NULL means <data_dir>/natives.
 * @asset_base_url is where the asset objects come from; NULL means
 * nuvasha_RESOLVE_ASSET_BASE_URL.
 *
 * The placeholders this module knows the value of are substituted in
 * both argument lists; the ones that depend on an account or on a window
 * are left as the document wrote them.
 *
 * Errors come from the download module while fetching, from nuvasha_MC_ERROR
 * while parsing a document and from nuvasha_RESOLVE_ERROR for everything
 * this module adds itself. */
NuvashaTaskScope *nuvasha_resolve_version_async (NuvashaVersionManifest *manifest,
                                         const char *version_id,
                                         const char *data_dir,
                                         const char *natives_dir,
                                         const char *asset_base_url,
                                         NuvashaResolveProgressFunc progress,
                                         gpointer progress_data,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/* (transfer full) */
NuvashaResolvedVersion *nuvasha_resolve_version_finish (GAsyncResult  *result,
                                                GError       **error);

/* The same work for a document that is not in @manifest: the loader
 * profile of an instance, already merged over the Minecraft version it
 * inherits from, which is what nuvasha_loaders_profile_finish() hands back.
 *
 * @version_id is the id of the merged document, which is what the client
 * jar and the asset index file are named after; @data_dir, @natives_dir
 * and @asset_base_url mean here what they mean in
 * nuvasha_resolve_version_async(). @description is not modified.
 *
 * @client_jar, when it is not NULL, is a jar on disk that runs instead
 * of the one the document names, which is what a loader that patches the
 * client installs; nothing is fetched for it and it has to exist. */
NuvashaTaskScope *nuvasha_resolve_description_async (NuvashaVersionDescription *description,
                                             const char *version_id,
                                             const char *client_jar,
                                             const char *data_dir,
                                             const char *natives_dir,
                                             const char *asset_base_url,
                                             NuvashaResolveProgressFunc progress,
                                             gpointer progress_data,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

G_END_DECLS

#endif /* nuvasha_MINECRAFT_RESOLVE_H */
