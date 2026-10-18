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
 * like every other asynchronous call in leppoisa the result arrives as a
 * GTask result on the caller's main context.
 *
 * Nothing here knows a version id: every id comes out of the manifest. */

#ifndef leppoisa_MINECRAFT_VERSIONS_H
#define leppoisa_MINECRAFT_VERSIONS_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "leppoisa-task.h"

G_BEGIN_DECLS

/* The address Mojang's own launcher uses. launchermeta.mojang.com
 * answers with the very same bytes for the older spelling. */
#define leppoisa_VERSION_MANIFEST_URL \
  "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

/* Directory below g_get_user_cache_dir() holding the manifest and the
 * version documents leppoisa has downloaded. */
#define leppoisa_VERSIONS_CACHE_SUBDIR "leppoisa/versions"

#define leppoisa_MC_ERROR (leppoisa_mc_error_quark ())

typedef enum {
  leppoisa_MC_ERROR_INVALID,    /* the document is not the metadata we expect */
  leppoisa_MC_ERROR_MISSING,    /* a required field is absent */
  leppoisa_MC_ERROR_NOT_FOUND,  /* the manifest knows no such version */
  leppoisa_MC_ERROR_CYCLE,      /* inheritsFrom walks in a circle */
  leppoisa_MC_ERROR_IO          /* the cache directory or file is unusable */
} LeppoisaMcError;

GQuark leppoisa_mc_error_quark (void);

/* The four values Mojang's "type" field can hold. */
typedef enum {
  leppoisa_MC_TYPE_RELEASE,
  leppoisa_MC_TYPE_SNAPSHOT,
  leppoisa_MC_TYPE_OLD_BETA,
  leppoisa_MC_TYPE_OLD_ALPHA
} LeppoisaMcVersionType;

/* FALSE for anything that is not one of the four type strings. */
gboolean leppoisa_mc_version_type_from_string (const char       *string,
                                          LeppoisaMcVersionType *type);

/* "release", "snapshot", "old_beta" or "old_alpha". */
const char *leppoisa_mc_version_type_to_string (LeppoisaMcVersionType type);

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
} LeppoisaDownloadInfo;

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
} LeppoisaRule;

/* What rules are evaluated against: this machine and the features the
 * launcher enables. A feature that is not set counts as false. */
typedef struct {
  char *os_name;
  char *os_arch;
  char *os_version;
  GHashTable *features; /* feature name -> set, never NULL */
} LeppoisaRuleContext;

/* A context describing this machine: "linux", the machine's
 * architecture and kernel version, no features. */
LeppoisaRuleContext *leppoisa_rule_context_new (void);

/* A context for tests and for anything that wants to describe another
 * machine. @os_arch and @os_version may be NULL. */
LeppoisaRuleContext *leppoisa_rule_context_new_for (const char *os_name,
                                          const char *os_arch,
                                          const char *os_version);

void leppoisa_rule_context_free (LeppoisaRuleContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LeppoisaRuleContext, leppoisa_rule_context_free)

void leppoisa_rule_context_set_feature (LeppoisaRuleContext *context,
                                   const char     *name,
                                   gboolean        value);

/* FALSE for a feature that was never set. */
gboolean leppoisa_rule_context_get_feature (LeppoisaRuleContext *context,
                                       const char     *name);

/* TRUE when every condition of @rule holds for @context. A rule
 * without conditions always matches. os.version is matched as a
 * regular expression over the whole kernel version; os.versionRange is
 * a Windows version selector and never matches here. */
gboolean leppoisa_rule_matches (const LeppoisaRule       *rule,
                           const LeppoisaRuleContext *context);

/* The verdict of a whole rule list, exactly as the format defines it:
 * no rules at all means allowed, otherwise the last rule that matches
 * decides, and when no rule matches the answer is disallowed. */
gboolean leppoisa_rules_allow (GPtrArray           *rules,
                          const LeppoisaRuleContext *context);

/* ---------------------------------------------------------- *
 * The contents of a version document
 * ---------------------------------------------------------- */

typedef struct {
  char *name;              /* "org.lwjgl:lwjgl:3.4.3" */
  LeppoisaDownloadInfo *artifact; /* NULL when the library has no jar */
  GHashTable *classifiers;   /* "natives-linux" -> LeppoisaDownloadInfo */
  GHashTable *natives;       /* "linux" -> "natives-linux" */
  GPtrArray *rules;          /* LeppoisaRule*, NULL when unconditional */
  GPtrArray *exclude;        /* "META-INF/", NULL when there is none */
} LeppoisaLibrary;

/* One entry of arguments.jvm or arguments.game: the words it stands
 * for and, when Mojang put rules on it, the rules deciding whether
 * they are used. @values holds at least one string. */
typedef struct {
  GPtrArray *rules;  /* LeppoisaRule*, NULL when the argument is plain */
  GPtrArray *values; /* char* */
} LeppoisaArgument;

typedef struct {
  char *component;   /* "java-runtime-epsilon" */
  int major_version; /* 25 */
} LeppoisaJavaVersion;

/* logging.client: which file to hand to log4j and where to get it. */
typedef struct {
  char *argument;
  char *type;
  LeppoisaDownloadInfo *file;
} LeppoisaLogging;

#define leppoisa_TYPE_VERSION_MANIFEST (leppoisa_version_manifest_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaVersionManifest, leppoisa_version_manifest,
                      leppoisa, VERSION_MANIFEST, GObject)

/* One row of the manifest. Owned by the manifest that holds it, so the
 * pointers stay valid exactly as long as it does. */
typedef struct {
  char *id;
  LeppoisaMcVersionType type;
  char *url;
  char *sha1;
  char *release_time;
} LeppoisaVersionEntry;

/* Read only. @entries holds LeppoisaVersionEntry* in the order Mojang
 * publishes them, which is the newest version first. */
struct _LeppoisaVersionManifest
{
  GObject parent_instance;

  char *latest_release;
  char *latest_snapshot;
  GPtrArray *entries;
};

/* Parses a manifest. @length may be -1 for a nul terminated document.
 * Every entry must carry id, type, url, sha1 and releaseTime, because
 * the entries are all this module needs to fetch and compare. */
LeppoisaVersionManifest *leppoisa_version_manifest_new_from_json (const char *json,
                                                        gssize      length,
                                                        GError    **error);

/* The entry with this id, or NULL. Borrowed from the manifest. */
const LeppoisaVersionEntry *leppoisa_version_manifest_find (LeppoisaVersionManifest *self,
                                                  const char         *id);

#define leppoisa_TYPE_VERSION_DESCRIPTION (leppoisa_version_description_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaVersionDescription, leppoisa_version_description,
                      leppoisa, VERSION_DESCRIPTION, GObject)

/* Read only: a parsed version document, or the result of merging one
 * over the version it inherits from. NULL members are fields the
 * document did not have; @jvm_arguments and @game_arguments are empty
 * arrays for a document that only has the legacy @minecraft_arguments. */
struct _LeppoisaVersionDescription
{
  GObject parent_instance;

  char *id;
  LeppoisaMcVersionType type;
  char *main_class;
  char *inherits_from; /* NULL when it inherits from nothing */

  char *assets;
  LeppoisaDownloadInfo *asset_index;
  LeppoisaDownloadInfo *client; /* downloads.client */

  GPtrArray *libraries;     /* LeppoisaLibrary* */
  GPtrArray *jvm_arguments; /* LeppoisaArgument* */
  GPtrArray *game_arguments; /* LeppoisaArgument* */
  gboolean modern_arguments; /* TRUE when "arguments" was there */
  char *minecraft_arguments; /* the legacy line, NULL for modern documents */

  LeppoisaLogging *logging;
  LeppoisaJavaVersion *java_version;
};

/* Parses one version document. @length may be -1. Only "id" and
 * "type" are required: a profile that inherits from another version
 * legitimately lacks the rest. */
LeppoisaVersionDescription *leppoisa_version_description_new_from_json (const char *json,
                                                              gssize      length,
                                                              GError    **error);

/* A new description holding @child on top of @parent: libraries and
 * arguments are concatenated with the parent first, every other field
 * the child carries overrides the parent's, and inherits_from becomes
 * the parent's so that a chain of profiles can be resolved step by
 * step. Neither argument is modified. */
LeppoisaVersionDescription *leppoisa_version_description_merge (LeppoisaVersionDescription *parent,
                                                      LeppoisaVersionDescription *child);

/* TRUE when @argument or @library is to be used on this machine. */
gboolean leppoisa_argument_allowed (const LeppoisaArgument *argument,
                               const LeppoisaRuleContext *context);
gboolean leppoisa_library_allowed (const LeppoisaLibrary *library,
                              const LeppoisaRuleContext *context);

/* The words of the legacy minecraftArguments line, split like a shell
 * command line. (transfer full) An empty array when the document does
 * not carry the line. */
GPtrArray *leppoisa_version_description_legacy_argv (LeppoisaVersionDescription *self);

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

/* Downloads the manifest at @url into the cache directory and parses
 * it. The copy on disk is replaced on every successful fetch and is
 * never used as a fallback: a list Mojang no longer serves is worse
 * than a visible failure the user can retry. */
LeppoisaTaskScope *leppoisa_version_manifest_fetch_async (const char *url,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);

/* (transfer full). Errors come from the download module while
 * fetching and from leppoisa_MC_ERROR while parsing. */
LeppoisaVersionManifest *leppoisa_version_manifest_fetch_finish (GAsyncResult *result,
                                                       GError     **error);

/* Fetches @version_id from @manifest with the sha1 the manifest
 * published, follows inheritsFrom up to the root and merges every step
 * into one description. Cached documents are verified against that sha1
 * before they are used, so a stale or damaged copy is downloaded again. */
LeppoisaTaskScope *leppoisa_version_load_async (LeppoisaVersionManifest *manifest,
                                      const char *version_id,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/* (transfer full). leppoisa_MC_ERROR_NOT_FOUND when the manifest has no
 * such version, leppoisa_MC_ERROR_CYCLE when inheritsFrom walks in a
 * circle, leppoisa_MC_ERROR_INVALID/MISSING for a document that will not
 * parse and leppoisa_MC_ERROR_NOT_FOUND when a parent is missing from the
 * manifest. Fetching failures keep the download error domain. */
LeppoisaVersionDescription *leppoisa_version_load_finish (GAsyncResult *result,
                                                GError     **error);

G_END_DECLS

#endif /* leppoisa_MINECRAFT_VERSIONS_H */
