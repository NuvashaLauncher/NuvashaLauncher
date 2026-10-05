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
 * like every other asynchronous call in AML the result arrives as a
 * GTask result on the caller's main context.
 *
 * Nothing here knows a version id: every id comes out of the manifest. */

#ifndef AML_MINECRAFT_VERSIONS_H
#define AML_MINECRAFT_VERSIONS_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* The address Mojang's own launcher uses. launchermeta.mojang.com
 * answers with the very same bytes for the older spelling. */
#define AML_VERSION_MANIFEST_URL \
  "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

/* Directory below g_get_user_cache_dir() holding the manifest and the
 * version documents AML has downloaded. */
#define AML_VERSIONS_CACHE_SUBDIR "AML"

#define AML_MC_ERROR (aml_mc_error_quark ())

typedef enum {
  AML_MC_ERROR_INVALID,    /* the document is not the metadata we expect */
  AML_MC_ERROR_MISSING,    /* a required field is absent */
  AML_MC_ERROR_NOT_FOUND,  /* the manifest knows no such version */
  AML_MC_ERROR_CYCLE,      /* inheritsFrom walks in a circle */
  AML_MC_ERROR_IO          /* the cache directory or file is unusable */
} AmlMcError;

GQuark aml_mc_error_quark (void);

/* The four values Mojang's "type" field can hold. */
typedef enum {
  AML_MC_TYPE_RELEASE,
  AML_MC_TYPE_SNAPSHOT,
  AML_MC_TYPE_OLD_BETA,
  AML_MC_TYPE_OLD_ALPHA
} AmlMcVersionType;

/* FALSE for anything that is not one of the four type strings. */
gboolean aml_mc_version_type_from_string (const char       *string,
                                          AmlMcVersionType *type);

/* "release", "snapshot", "old_beta" or "old_alpha". */
const char *aml_mc_version_type_to_string (AmlMcVersionType type);

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
} AmlDownloadInfo;

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
} AmlRule;

/* What rules are evaluated against: this machine and the features the
 * launcher enables. A feature that is not set counts as false. */
typedef struct {
  char *os_name;
  char *os_arch;
  char *os_version;
  GHashTable *features; /* feature name -> set, never NULL */
} AmlRuleContext;

/* A context describing this machine: "linux", the machine's
 * architecture and kernel version, no features. */
AmlRuleContext *aml_rule_context_new (void);

/* A context for tests and for anything that wants to describe another
 * machine. @os_arch and @os_version may be NULL. */
AmlRuleContext *aml_rule_context_new_for (const char *os_name,
                                          const char *os_arch,
                                          const char *os_version);

void aml_rule_context_free (AmlRuleContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlRuleContext, aml_rule_context_free)

void aml_rule_context_set_feature (AmlRuleContext *context,
                                   const char     *name,
                                   gboolean        value);

/* FALSE for a feature that was never set. */
gboolean aml_rule_context_get_feature (AmlRuleContext *context,
                                       const char     *name);

/* TRUE when every condition of @rule holds for @context. A rule
 * without conditions always matches. os.version is matched as a
 * regular expression over the whole kernel version; os.versionRange is
 * a Windows version selector and never matches here. */
gboolean aml_rule_matches (const AmlRule       *rule,
                           const AmlRuleContext *context);

/* The verdict of a whole rule list, exactly as the format defines it:
 * no rules at all means allowed, otherwise the last rule that matches
 * decides, and when no rule matches the answer is disallowed. */
gboolean aml_rules_allow (GPtrArray           *rules,
                          const AmlRuleContext *context);

/* ---------------------------------------------------------- *
 * The contents of a version document
 * ---------------------------------------------------------- */

typedef struct {
  char *name;              /* "org.lwjgl:lwjgl:3.4.3" */
  AmlDownloadInfo *artifact; /* NULL when the library has no jar */
  GHashTable *classifiers;   /* "natives-linux" -> AmlDownloadInfo */
  GHashTable *natives;       /* "linux" -> "natives-linux" */
  GPtrArray *rules;          /* AmlRule*, NULL when unconditional */
  GPtrArray *exclude;        /* "META-INF/", NULL when there is none */
} AmlLibrary;

/* One entry of arguments.jvm or arguments.game: the words it stands
 * for and, when Mojang put rules on it, the rules deciding whether
 * they are used. @values holds at least one string. */
typedef struct {
  GPtrArray *rules;  /* AmlRule*, NULL when the argument is plain */
  GPtrArray *values; /* char* */
} AmlArgument;

typedef struct {
  char *component;   /* "java-runtime-epsilon" */
  int major_version; /* 25 */
} AmlJavaVersion;

/* logging.client: which file to hand to log4j and where to get it. */
typedef struct {
  char *argument;
  char *type;
  AmlDownloadInfo *file;
} AmlLogging;

#define AML_TYPE_VERSION_MANIFEST (aml_version_manifest_get_type ())

G_DECLARE_FINAL_TYPE (AmlVersionManifest, aml_version_manifest,
                      AML, VERSION_MANIFEST, GObject)

/* One row of the manifest. Owned by the manifest that holds it, so the
 * pointers stay valid exactly as long as it does. */
typedef struct {
  char *id;
  AmlMcVersionType type;
  char *url;
  char *sha1;
  char *release_time;
} AmlVersionEntry;

/* Read only. @entries holds AmlVersionEntry* in the order Mojang
 * publishes them, which is the newest version first. */
struct _AmlVersionManifest
{
  GObject parent_instance;

  char *latest_release;
  char *latest_snapshot;
  GPtrArray *entries;
};

/* Parses a manifest. @length may be -1 for a nul terminated document.
 * Every entry must carry id, type, url, sha1 and releaseTime, because
 * the entries are all this module needs to fetch and compare. */
AmlVersionManifest *aml_version_manifest_new_from_json (const char *json,
                                                        gssize      length,
                                                        GError    **error);

/* The entry with this id, or NULL. Borrowed from the manifest. */
const AmlVersionEntry *aml_version_manifest_find (AmlVersionManifest *self,
                                                  const char         *id);

#define AML_TYPE_VERSION_DESCRIPTION (aml_version_description_get_type ())

G_DECLARE_FINAL_TYPE (AmlVersionDescription, aml_version_description,
                      AML, VERSION_DESCRIPTION, GObject)

/* Read only: a parsed version document, or the result of merging one
 * over the version it inherits from. NULL members are fields the
 * document did not have; @jvm_arguments and @game_arguments are empty
 * arrays for a document that only has the legacy @minecraft_arguments. */
struct _AmlVersionDescription
{
  GObject parent_instance;

  char *id;
  AmlMcVersionType type;
  char *main_class;
  char *inherits_from; /* NULL when it inherits from nothing */

  char *assets;
  AmlDownloadInfo *asset_index;
  AmlDownloadInfo *client; /* downloads.client */

  GPtrArray *libraries;     /* AmlLibrary* */
  GPtrArray *jvm_arguments; /* AmlArgument* */
  GPtrArray *game_arguments; /* AmlArgument* */
  gboolean modern_arguments; /* TRUE when "arguments" was there */
  char *minecraft_arguments; /* the legacy line, NULL for modern documents */

  AmlLogging *logging;
  AmlJavaVersion *java_version;
};

/* Parses one version document. @length may be -1. Only "id" and
 * "type" are required: a profile that inherits from another version
 * legitimately lacks the rest. */
AmlVersionDescription *aml_version_description_new_from_json (const char *json,
                                                              gssize      length,
                                                              GError    **error);

/* A new description holding @child on top of @parent: libraries and
 * arguments are concatenated with the parent first, every other field
 * the child carries overrides the parent's, and inherits_from becomes
 * the parent's so that a chain of profiles can be resolved step by
 * step. Neither argument is modified. */
AmlVersionDescription *aml_version_description_merge (AmlVersionDescription *parent,
                                                      AmlVersionDescription *child);

/* TRUE when @argument or @library is to be used on this machine. */
gboolean aml_argument_allowed (const AmlArgument *argument,
                               const AmlRuleContext *context);
gboolean aml_library_allowed (const AmlLibrary *library,
                              const AmlRuleContext *context);

/* The words of the legacy minecraftArguments line, split like a shell
 * command line. (transfer full) An empty array when the document does
 * not carry the line. */
GPtrArray *aml_version_description_legacy_argv (AmlVersionDescription *self);

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

/* Downloads the manifest at @url into the cache directory and parses
 * it. The copy on disk is replaced on every successful fetch and is
 * never used as a fallback: a list Mojang no longer serves is worse
 * than a visible failure the user can retry. */
void aml_version_manifest_fetch_async (const char         *url,
                                       GCancellable       *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer            user_data);

/* (transfer full). Errors come from the download module while
 * fetching and from AML_MC_ERROR while parsing. */
AmlVersionManifest *aml_version_manifest_fetch_finish (GAsyncResult *result,
                                                       GError     **error);

/* Fetches @version_id from @manifest with the sha1 the manifest
 * published, follows inheritsFrom up to the root and merges every step
 * into one description. Cached documents are verified against that sha1
 * before they are used, so a stale or damaged copy is downloaded again. */
void aml_version_load_async (AmlVersionManifest *manifest,
                             const char         *version_id,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data);

/* (transfer full). AML_MC_ERROR_NOT_FOUND when the manifest has no
 * such version, AML_MC_ERROR_CYCLE when inheritsFrom walks in a
 * circle, AML_MC_ERROR_INVALID/MISSING for a document that will not
 * parse and AML_MC_ERROR_NOT_FOUND when a parent is missing from the
 * manifest. Fetching failures keep the download error domain. */
AmlVersionDescription *aml_version_load_finish (GAsyncResult *result,
                                                GError     **error);

G_END_DECLS

#endif /* AML_MINECRAFT_VERSIONS_H */
