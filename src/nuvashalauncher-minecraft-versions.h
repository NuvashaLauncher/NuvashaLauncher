/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Mojang's launcher metadata: the version manifest v2, the version
 * documents it lists, their inheritance and their rules.
 *
 * Parsing and rule evaluation are plain, synchronous work on data that
 * is already in memory: no widget, no thread and no hidden state. The
 * two asynchronous calls at the end fetch those documents through the
 * download module and keep a copy under the user's cache directory;
 * like every other asynchronous call in nuvashalauncher the result arrives as a
 * GTask result on the caller's main context.
 *
 * Nothing here knows a version id: every id comes out of the manifest. */

#ifndef nuvashalauncher_MINECRAFT_VERSIONS_H
#define nuvashalauncher_MINECRAFT_VERSIONS_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

#define nuvashalauncher_VERSION_MANIFEST_URL \
  "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

/* Directory below g_get_user_cache_dir() holding the manifest and the
 * version documents nuvashalauncher has downloaded. */
#define nuvashalauncher_VERSIONS_CACHE_SUBDIR "nuvashalauncher/versions"

#define nuvashalauncher_MC_ERROR (nuvashalauncher_mc_error_quark ())

typedef enum {
  nuvashalauncher_MC_ERROR_INVALID,    /* the document is not the metadata we expect */
  nuvashalauncher_MC_ERROR_MISSING,    /* a required field is absent */
  nuvashalauncher_MC_ERROR_NOT_FOUND,  /* the manifest knows no such version */
  nuvashalauncher_MC_ERROR_CYCLE,      /* inheritsFrom walks in a circle */
  nuvashalauncher_MC_ERROR_IO          /* the cache directory or file is unusable */
} nuvashalauncherMcError;

GQuark nuvashalauncher_mc_error_quark (void);

/* The four values Mojang's "type" field can hold. */
typedef enum {
  nuvashalauncher_MC_TYPE_RELEASE,
  nuvashalauncher_MC_TYPE_SNAPSHOT,
  nuvashalauncher_MC_TYPE_OLD_BETA,
  nuvashalauncher_MC_TYPE_OLD_ALPHA
} nuvashalauncherMcVersionType;

gboolean nuvashalauncher_mc_version_type_from_string (const char       *string,
                                          nuvashalauncherMcVersionType *type);

/* "release", "snapshot", "old_beta" or "old_alpha". */
const char *nuvashalauncher_mc_version_type_to_string (nuvashalauncherMcVersionType type);

/* ---------------------------------------------------------- *
 * One file the manifest points at: a jar, an asset index, the
 * logging configuration. Owned by whoever parsed the document it
 * came from, so the pointers are borrowed and never NULL unless the
 * field was optional and absent. @path, @id and @total_size are only
 * there where Mojang writes them.
 * ---------------------------------------------------------- */

typedef struct {
  char *id;
  char *url;
  char *sha1;
  char *path;
  gint64 size;
  gint64 total_size;
} nuvashalauncherDownloadInfo;

/* ---------------------------------------------------------- *
 * Rules: "allow" or "disallow", an optional OS description and an
 * optional set of launcher features. The OS fields are NULL when the
 * rule does not mention them.
 * ---------------------------------------------------------- */

typedef struct {
  gboolean allow;
  char *os_name;     /* "linux", "windows", "osx" */
  char *os_arch;
  char *os_version;  /* regular expression, matched over the whole value */
  GHashTable *features; /* feature name -> value the rule asks for,
                         * NULL when it mentions no feature */
} nuvashalauncherRule;

/* Machine properties and enabled features used to evaluate rules. */
typedef struct {
  char *os_name;
  char *os_arch;
  char *os_version;
  GHashTable *features; /* feature name -> set, never NULL */
} nuvashalauncherRuleContext;

/* A context describing this machine: "linux", the machine's
 * architecture and kernel version, no features. */
nuvashalauncherRuleContext *nuvashalauncher_rule_context_new (void);

/* A context for tests and for anything that wants to describe another
 * machine. @os_arch and @os_version may be NULL. */
nuvashalauncherRuleContext *nuvashalauncher_rule_context_new_for (const char *os_name,
                                          const char *os_arch,
                                          const char *os_version);

void nuvashalauncher_rule_context_free (nuvashalauncherRuleContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherRuleContext, nuvashalauncher_rule_context_free)

void nuvashalauncher_rule_context_set_feature (nuvashalauncherRuleContext *context,
                                   const char     *name,
                                   gboolean        value);

gboolean nuvashalauncher_rule_context_get_feature (nuvashalauncherRuleContext *context,
                                       const char     *name);

/* TRUE when every condition of @rule holds for @context. A rule
 * without conditions always matches. os.version is matched as a
 * regular expression over the whole kernel version; os.versionRange is
 * a Windows version selector and never matches here. */
gboolean nuvashalauncher_rule_matches (const nuvashalauncherRule       *rule,
                           const nuvashalauncherRuleContext *context);

/* The verdict of a whole rule list, exactly as the format defines it:
 * no rules at all means allowed, otherwise the last rule that matches
 * decides, and when no rule matches the answer is disallowed. */
gboolean nuvashalauncher_rules_allow (GPtrArray           *rules,
                          const nuvashalauncherRuleContext *context);

/* ---------------------------------------------------------- *
 * The contents of a version document
 * ---------------------------------------------------------- */

typedef struct {
  char *name;              /* "org.lwjgl:lwjgl:3.4.3" */
  nuvashalauncherDownloadInfo *artifact; /* NULL when the library has no jar */
  GHashTable *classifiers;   /* "natives-linux" -> nuvashalauncherDownloadInfo */
  GHashTable *natives;       /* "linux" -> "natives-linux" */
  GPtrArray *rules;          /* nuvashalauncherRule*, NULL when unconditional */
  GPtrArray *exclude;        /* "META-INF/", NULL when there is none */
} nuvashalauncherLibrary;

/* One entry of arguments.jvm or arguments.game: the words it stands
 * for and, when Mojang put rules on it, the rules deciding whether
 * they are used. @values holds at least one string. */
typedef struct {
  GPtrArray *rules;  /* nuvashalauncherRule*, NULL when the argument is plain */
  GPtrArray *values; /* char* */
} nuvashalauncherArgument;

typedef struct {
  char *component;   /* "java-runtime-epsilon" */
  int major_version; /* 25 */
} nuvashalauncherJavaVersion;

/* logging.client: which file to hand to log4j and where to get it. */
typedef struct {
  char *argument;
  char *type;
  nuvashalauncherDownloadInfo *file;
} nuvashalauncherLogging;

#define nuvashalauncher_TYPE_VERSION_MANIFEST (nuvashalauncher_version_manifest_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherVersionManifest, nuvashalauncher_version_manifest,
                      nuvashalauncher, VERSION_MANIFEST, GObject)

/* One manifest entry, owned by its manifest. */
typedef struct {
  char *id;
  nuvashalauncherMcVersionType type;
  char *url;
  char *sha1;
  char *release_time;
} nuvashalauncherVersionEntry;

/* Read only. @entries holds nuvashalauncherVersionEntry* in the order Mojang
 * publishes them, which is the newest version first. */
struct _nuvashalauncherVersionManifest
{
  GObject parent_instance;

  char *latest_release;
  char *latest_snapshot;
  GPtrArray *entries;
};

/* Parses a manifest. @length may be -1 for a nul terminated document.
 * Every entry must carry id, type, url, sha1 and releaseTime, because
 * the entries are all this module needs to fetch and compare. */
nuvashalauncherVersionManifest *nuvashalauncher_version_manifest_new_from_json (const char *json,
                                                        gssize      length,
                                                        GError    **error);

/* The entry with this id, or NULL. Borrowed from the manifest. */
const nuvashalauncherVersionEntry *nuvashalauncher_version_manifest_find (nuvashalauncherVersionManifest *self,
                                                  const char         *id);

#define nuvashalauncher_TYPE_VERSION_DESCRIPTION (nuvashalauncher_version_description_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherVersionDescription, nuvashalauncher_version_description,
                      nuvashalauncher, VERSION_DESCRIPTION, GObject)

/* Read only: a parsed version document, or the result of merging one
 * over the version it inherits from. NULL members are fields the
 * document did not have; @jvm_arguments and @game_arguments are empty
 * arrays for a document that only has the legacy @minecraft_arguments. */
struct _nuvashalauncherVersionDescription
{
  GObject parent_instance;

  char *id;
  nuvashalauncherMcVersionType type;
  char *main_class;
  char *inherits_from;

  char *assets;
  nuvashalauncherDownloadInfo *asset_index;
  nuvashalauncherDownloadInfo *client; /* downloads.client */

  GPtrArray *libraries;     /* nuvashalauncherLibrary* */
  GPtrArray *jvm_arguments; /* nuvashalauncherArgument* */
  GPtrArray *game_arguments; /* nuvashalauncherArgument* */
  gboolean modern_arguments; /* TRUE when "arguments" was there */
  char *minecraft_arguments; /* the legacy line, NULL for modern documents */

  nuvashalauncherLogging *logging;
  nuvashalauncherJavaVersion *java_version;
};

/* Parses one version document. @length may be -1. Only "id" and
 * "type" are required: a profile that inherits from another version
 * legitimately lacks the rest. */
nuvashalauncherVersionDescription *nuvashalauncher_version_description_new_from_json (const char *json,
                                                              gssize      length,
                                                              GError    **error);

/* A new description holding @child on top of @parent: libraries and
 * arguments are concatenated with the parent first, every other field
 * the child carries overrides the parent's, and inherits_from becomes
 * the parent's so that a chain of profiles can be resolved step by
 * step. Neither argument is modified. */
nuvashalauncherVersionDescription *nuvashalauncher_version_description_merge (nuvashalauncherVersionDescription *parent,
                                                      nuvashalauncherVersionDescription *child);

/* TRUE when @argument or @library is to be used on this machine. */
gboolean nuvashalauncher_argument_allowed (const nuvashalauncherArgument *argument,
                               const nuvashalauncherRuleContext *context);
gboolean nuvashalauncher_library_allowed (const nuvashalauncherLibrary *library,
                              const nuvashalauncherRuleContext *context);

/* The words of the legacy minecraftArguments line, split like a shell
 * command line. (transfer full) An empty array when the document does
 * not carry the line. */
GPtrArray *nuvashalauncher_version_description_legacy_argv (nuvashalauncherVersionDescription *self);

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

/* Downloads the manifest at @url into the cache directory and parses
 * it. The copy on disk is replaced on every successful fetch and is
 * never used as a fallback: a list Mojang no longer serves is worse
 * than a visible failure the user can retry. */
nuvashalauncherTaskScope *nuvashalauncher_version_manifest_fetch_async (const char *url,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);

/* (transfer full). Errors come from the download module while
 * fetching and from nuvashalauncher_MC_ERROR while parsing. */
nuvashalauncherVersionManifest *nuvashalauncher_version_manifest_fetch_finish (GAsyncResult *result,
                                                       GError     **error);

/* Fetches @version_id from @manifest with the sha1 the manifest
 * published, follows inheritsFrom up to the root and merges every step
 * into one description. Cached documents are verified against that sha1
 * before they are used, so a stale or damaged copy is downloaded again. */
nuvashalauncherTaskScope *nuvashalauncher_version_load_async (nuvashalauncherVersionManifest *manifest,
                                      const char *version_id,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/* (transfer full). nuvashalauncher_MC_ERROR_NOT_FOUND when the manifest has no
 * such version, nuvashalauncher_MC_ERROR_CYCLE when inheritsFrom walks in a
 * circle, nuvashalauncher_MC_ERROR_INVALID/MISSING for a document that will not
 * parse and nuvashalauncher_MC_ERROR_NOT_FOUND when a parent is missing from the
 * manifest. Fetching failures keep the download error domain. */
nuvashalauncherVersionDescription *nuvashalauncher_version_load_finish (GAsyncResult *result,
                                                GError     **error);

G_END_DECLS

#endif /* nuvashalauncher_MINECRAFT_VERSIONS_H */
