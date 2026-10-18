/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyModrinth: reading the Modrinth API v2 for the three kinds of
 * content an instance carries: mods, resource packs and shader packs.
 *
 * Modrinth is the only provider comfy asks. There is no provider selector
 * and no abstraction behind it: this module speaks the Modrinth API.
 *
 * Compatibility is decided here and never shown to a user afterwards:
 * a mod is only ever listed for the Minecraft version and the loader of
 * the instance it would go into, and a resource pack or a shader pack
 * only for the Minecraft version. A version that does not fit never
 * reaches a list, so nothing can be installed that the game would
 * refuse to load.
 *
 * Everything is a plain data object built from a JSON answer, and every
 * function that has to parse an answer also exists in a form a test can
 * hand a fixture to. */

#ifndef comfy_MODRINTH_H
#define comfy_MODRINTH_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "comfy-task.h"

G_BEGIN_DECLS

/* The Modrinth API v2. */
#define comfy_MODRINTH_URL "https://api.modrinth.com/v2"

/* Modrinth asks every client to name itself, and to carry an address its
 * operators can reach it at if they need to. */
#define comfy_MODRINTH_USER_AGENT \
  "comfy (https://gitlab.com/comfylauncher/comfy)"

#define comfy_MODRINTH_ERROR (comfy_modrinth_error_quark ())

typedef enum {
  comfy_MODRINTH_ERROR_NETWORK,   /* the API could not be reached */
  comfy_MODRINTH_ERROR_HTTP,      /* an HTTP status that is not success */
  comfy_MODRINTH_ERROR_RATE_LIMIT, /* HTTP 429, too many requests */
  comfy_MODRINTH_ERROR_PARSE,     /* the answer was not what it must be */
  comfy_MODRINTH_ERROR_NOT_FOUND  /* no such project, version or hash */
} ComfyModrinthError;

GQuark comfy_modrinth_error_quark (void);
char *comfy_modrinth_json_node_to_string (JsonNode *node);

/* ---------------------------------------------------------- *
 * The kinds of content
 * ---------------------------------------------------------- */

/* What a content page shows. Mods need a loader, the other two only
 * need the Minecraft version. */
typedef enum {
  comfy_CONTENT_MOD,
  comfy_CONTENT_RESOURCE_PACK,
  comfy_CONTENT_SHADER_PACK,
} ComfyContentType;

/* What Modrinth calls this kind of content in a facet: "mod",
 * "resourcepack" or "shader". Never NULL. */
const char *comfy_content_type_to_string (ComfyContentType type);

/* What the page and the rows are titled: "Mods", "Resource Packs" or
 * "Shader Packs". Never NULL. */
const char *comfy_content_type_title (ComfyContentType type);

/* The folder of the instance this content goes in: "mods",
 * "resourcepacks" or "shaderpacks". Never NULL. */
const char *comfy_content_type_folder (ComfyContentType type);

/* TRUE for the kind that only runs on a mod loader. */
gboolean comfy_content_type_needs_loader (ComfyContentType type);

/* The file extension of this content: ".jar" for a mod, ".zip" for the
 * other two. Never NULL. */
const char *comfy_content_type_extension (ComfyContentType type);

/* The symbolic icon the instance page shows for this content. Never
 * NULL. */
const char *comfy_content_type_icon (ComfyContentType type);

/* ---------------------------------------------------------- *
 * What a search answers
 * ---------------------------------------------------------- */

typedef struct {
  char *project_id;  /* the id Modrinth addresses it by */
  char *slug;
  char *title;
  char *description;
  char *author;
  char *icon_url;
  guint64 downloads;
  guint64 followers;

  /* What the /search answer also said: every Minecraft version the
   * project lists itself for, and what it tags itself with. */
  GPtrArray *versions;   /* const char*, never NULL */
  GPtrArray *categories; /* const char*, never NULL */
} ComfyModrinthProject;

void comfy_modrinth_project_free (ComfyModrinthProject *project);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthProject, comfy_modrinth_project_free)

/* A copy of @project, so that a list of rows can own its own of what it
 * shows. (transfer full) */
ComfyModrinthProject *comfy_modrinth_project_copy (ComfyModrinthProject *project);

/* What one search asked for. @loader is NULL for the content that does
 * not need one, and @categories is a GPtrArray of const char* that may
 * be NULL. */
typedef struct {
  ComfyContentType type;
  char *query;            /* "" asks for everything */
  char *minecraft_version;
  char *loader;           /* "fabric", "forge", ... or NULL */
  GPtrArray *categories;  /* (element-free) const char* */
  guint offset;
  guint limit;
} ComfyModrinthSearch;

void comfy_modrinth_search_free (ComfyModrinthSearch *search);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthSearch, comfy_modrinth_search_free)

/* What a search answers: the projects that matched and how many there
 * are in all, which is what the pagination is read from. */
typedef struct {
  GPtrArray *projects; /* ComfyModrinthProject*, never NULL */
  guint64 total_hits;
} ComfyModrinthResults;

void comfy_modrinth_results_free (ComfyModrinthResults *results);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthResults, comfy_modrinth_results_free)

/* The URL of a search, built from its facets and its pagination. The
 * facets are what makes the answer compatible with the instance: the
 * project type, and for a mod the Minecraft version and the loader.
 *
 * (transfer full). What a test builds and what the client asks with. */
char *comfy_modrinth_search_url (const ComfyModrinthSearch *search);

/* The same facets on their own, the way Modrinth wants them: an array
 * of arrays of "name:value" strings.
 *
 * (transfer full). */
char *comfy_modrinth_facets (const ComfyModrinthSearch *search);

/* Searches. The answers are only the projects that fit the Minecraft
 * version and the loader the search asked for. */
ComfyTaskScope *comfy_modrinth_search_async (const ComfyModrinthSearch *search,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

/* (transfer full). NULL on error. */
ComfyModrinthResults *comfy_modrinth_search_finish (GAsyncResult  *result,
                                                          GError       **error);

ComfyTaskScope *comfy_modrinth_version_by_id_async (const char *version_id,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);

/* The projects out of what /search answered, and the total it counted.
 * This is what the fetch above parses, and what a test hands a fixture
 * to. (transfer full) */
ComfyModrinthResults *comfy_modrinth_results_from_json (const char *json,
                                                    gssize      length,
                                                    GError     **error);

/* ---------------------------------------------------------- *
 * What a project answers
 * ---------------------------------------------------------- */

/* One project in full: what a detail page shows. */
typedef struct {
  char *project_id;
  char *slug;
  char *title;
  char *description;
  char *author;
  char *icon_url;
  char *body;          /* the description as Markdown */
  char *license;
  char *source_url;
  char *issues_url;
  char *wiki_url;
  guint64 downloads;
  guint64 followers;
  GPtrArray *game_versions; /* const char* */
  GPtrArray *categories;    /* const char* */
  GPtrArray *loaders;       /* const char* */
} ComfyModrinthDetails;

void comfy_modrinth_details_free (ComfyModrinthDetails *details);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthDetails, comfy_modrinth_details_free)

ComfyTaskScope *comfy_modrinth_details_async (const char *project_id,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

/* (transfer full). NULL on error. */
ComfyModrinthDetails *comfy_modrinth_details_finish (GAsyncResult  *result,
                                                 GError       **error);

/* One project out of what /project answered. (transfer full) */
ComfyModrinthDetails *comfy_modrinth_details_from_json (const char *json,
                                                   gssize      length,
                                                   GError     **error);

/* ---------------------------------------------------------- *
 * What a version answers
 * ---------------------------------------------------------- */

typedef struct {
  char *filename;
  char *url;
  char *sha1;
  char *sha512;
  goffset size;
  gboolean primary;
} ComfyModrinthFile;

void comfy_modrinth_file_free (ComfyModrinthFile *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthFile, comfy_modrinth_file_free)

/* What a version depends on, as Modrinth publishes it. */
typedef enum {
  comfy_DEPENDENCY_REQUIRED,      /* the version does not run without it */
  comfy_DEPENDENCY_OPTIONAL,      /* it runs better with it */
  comfy_DEPENDENCY_EMBEDDED,      /* already inside this file */
  comfy_DEPENDENCY_INCOMPATIBLE   /* it does not run with this */
} ComfyModrinthDependencyType;

const char *comfy_modrinth_dependency_to_string (ComfyModrinthDependencyType type);

typedef struct {
  ComfyModrinthDependencyType type;
  char *project_id;  /* NULL: only a file name was given */
  char *version_id;  /* NULL: any version of the project will do */
  char *file_name;   /* NULL: the primary file of the project */
} ComfyModrinthDependency;

void comfy_modrinth_dependency_free (ComfyModrinthDependency *dependency);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthDependency,
                               comfy_modrinth_dependency_free)

/* A copy of @dependency, so that a caller can hold it without keeping
 * the version it came from alive. (transfer full) */
ComfyModrinthDependency *comfy_modrinth_dependency_copy (
    ComfyModrinthDependency *dependency);

/* One published version of a project: the files it is made of and what
 * it needs beside them. */
typedef struct {
  char *version_id;
  char *project_id;
  char *name;
  char *version_number;
  char *changelog;
  char *date_published;
  guint64 downloads;
  GPtrArray *game_versions;  /* const char* */
  GPtrArray *loaders;        /* const char* */
  GPtrArray *files;          /* ComfyModrinthFile* */
  GPtrArray *dependencies;   /* ComfyModrinthDependency* */
} ComfyModrinthVersion;

void comfy_modrinth_version_free (ComfyModrinthVersion *version);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthVersion, comfy_modrinth_version_free)

/* A copy of @version, so that something can be kept after the list it
 * was read from is gone. (transfer full) */
ComfyModrinthVersion *comfy_modrinth_version_copy (ComfyModrinthVersion *version);

/* The file an install writes: the primary one, or the first when the
 * version marks none as primary. NULL when the version holds no file.
 * Borrowed from @version. */
ComfyModrinthFile *comfy_modrinth_version_primary_file (ComfyModrinthVersion *version);

/* TRUE when @version carries a file whose sha1 is @sha1: the update
 * endpoint answers with the newest version that fits, and that version
 * can be the one already installed. */
gboolean comfy_modrinth_version_has_hash (const ComfyModrinthVersion *version,
                                        const char               *sha1);

/* The versions of @project_id that run @minecraft_version and, for a
 * mod, @loader, newest first.
 *
 * @loader is NULL for the content that does not need one. A version
 * that does not fit is left out here rather than shown and refused
 * later.
 *
 * comfy_MODRINTH_ERROR_NOT_FOUND when the project publishes nothing for
 * that Minecraft version and loader, which is what the page shows as
 * "No compatible version found for Minecraft <version> and <Loader>." */
ComfyTaskScope *comfy_modrinth_versions_async (const char *project_id,
                                           ComfyContentType type,
                                           const char *minecraft_version,
                                           const char *loader,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

/* (transfer full) the ComfyModrinthVersion* of what fits, newest first.
 * NULL on error. */
GPtrArray *comfy_modrinth_versions_finish (GAsyncResult  *result,
                                         GError       **error);

/* One category Modrinth publishes for a project type.
 *
 * @name is the slug, and it is what a facet is built from, so it is sent
 * exactly as Modrinth wrote it. @header is the group Modrinth files the
 * category under ("features", "categories", "resolutions"), which is
 * what a popover lists the categories under; it may be NULL. */
typedef struct {
  char *name;
  char *header;
} ComfyModrinthCategory;

void comfy_modrinth_category_free (ComfyModrinthCategory *category);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyModrinthCategory,
                               comfy_modrinth_category_free)

/* What a user reads for @name: "colored-lighting" becomes "Colored
 * Lighting" and "512x+" stays "512x+", because a number with a sign is
 * not a phrase to be capitalised. (transfer full) */
char *comfy_modrinth_category_label (const char *name);

/* What Modrinth calls a kind of project in the tags endpoint: "mod",
 * "resourcepack", "shader" or "modpack". (transfer full) */
char *comfy_modrinth_project_type_for_content (ComfyContentType type);

/* The categories Modrinth publishes for @project_type, which is what
 * the filter of a Browse page offers. They come from the tags projects
 * are tagged with, so the list is Modrinth's own and not one comfy made
 * up.
 *
 * The whole tag list is fetched once and kept for as long as the process
 * runs: every page that opens afterwards is answered from that, so
 * opening a second instance does not ask Modrinth for the same 126 tags
 * again. A caller that waits while the fetch is in flight and cancels
 * stops waiting and is answered as cancelled; the fetch itself carries
 * on for the others. */
ComfyTaskScope *comfy_modrinth_categories_async (const char *project_type,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

/* (transfer full) the ComfyModrinthCategory*, sorted by name. NULL on
 * error. */
GPtrArray *comfy_modrinth_categories_finish (GAsyncResult  *result,
                                           GError       **error);

/* The categories out of what the tags endpoint answered, the ones of
 * @project_type only, sorted by name. This is what the fetch above
 * parses, and what a test hands a fixture to. (transfer full) */
GPtrArray *comfy_modrinth_categories_from_json (const char     *json,
                                              gssize          length,
                                              const char     *project_type,
                                              GError        **error);

/* The same, out of what /project/<id>/version answered: what fits, and
 * only that, newest first. (transfer full) */
GPtrArray *comfy_modrinth_versions_from_json (const char  *json,
                                            gssize       length,
                                            ComfyContentType    type,
                                            const char  *minecraft_version,
                                            const char  *loader,
                                            GError      **error);

/* One version out of one version object. NULL when the answer is not a
 * version with a file to install. What a hash lookup and an update check
 * both parse. */
ComfyModrinthVersion *comfy_modrinth_version_from_json (const char  *json,
                                                   gssize       length,
                                                   GError     **error);

/* The one rule of compatibility, and the reason nothing incompatible is
 * ever listed.
 *
 * A mod runs on one Minecraft version and one loader. A resource pack
 * and a shader pack are loaded by the game itself, so the Minecraft
 * version is all that decides them. FALSE means the version must not be
 * offered for that instance. */
gboolean comfy_modrinth_version_fits (ComfyModrinthVersion *version,
                                    ComfyContentType      type,
                                    const char         *minecraft_version,
                                    const char         *loader);

/* The words a user reads when a project publishes nothing that fits the
 * instance: "No compatible version found for Minecraft 1.20.1 and
 * Fabric." for a mod and the same without the loader for the other two.
 * (transfer full) */
char *comfy_modrinth_no_compatible_message (ComfyContentType type,
                                          const char    *minecraft_version,
                                          const char    *loader);

/* Which project, and which version, a file belongs to.
 *
 * comfy_MODRINTH_ERROR_NOT_FOUND when Modrinth does not know the hash:
 * the file is not from Modrinth, or is not the exact file it
 * published, and comfy says so rather than guessing. */
ComfyTaskScope *comfy_modrinth_version_by_hash_async (const char *sha1,
                                                   GAsyncReadyCallback callback,
                                                   gpointer user_data);

/* (transfer full). NULL on error. */
ComfyModrinthVersion *comfy_modrinth_version_by_hash_finish (GAsyncResult  *result,
                                                         GError       **error);

/* A newer version of the project the file @sha1 belongs to, for
 * @minecraft_version and @loader.
 *
 * This is Modrinth's hash update endpoint, so the question asked is
 * "is there something newer for these exact files", not "is the newest
 * version of the project newer than the file on disk".
 *
 * NULL is the answer when Modrinth has nothing newer that fits, which
 * is what a page shows as up to date; it is not an error. */
ComfyTaskScope *comfy_modrinth_update_by_hash_async (ComfyContentType type,
                                                   const char *sha1,
                                                   const char *minecraft_version,
                                                   const char *loader,
                                                   GAsyncReadyCallback callback,
                                                   gpointer user_data);

/* (transfer full). NULL when there is no update, and on error. */
ComfyModrinthVersion *comfy_modrinth_update_by_hash_finish (GAsyncResult  *result,
                                                         GError       **error);

/* ---------------------------------------------------------- *
 * What an install of a version needs
 * ---------------------------------------------------------- */

/* The dependencies of @version that an install still has to fetch: the
 * ones it requires and that @installed does not have yet, each one only
 * once. Embedded, optional and incompatible dependencies are not in the
 * result: the first is inside the file, the second is a choice and the
 * third is a conflict.
 *
 * @installed is a set of project ids (g_str_hash) and may be NULL,
 * which reads as "nothing is installed". @conflicts, when not NULL, is
 * filled with the dependencies @version says it does not run with, and
 * only for the projects that are in @installed.
 *
 * comfy_MODRINTH_ERROR_PARSE when @version requires a dependency that
 * names no project, which is the one dependency that cannot be
 * fetched.
 *
 * (transfer full) the ComfyModrinthDependency*. */
GPtrArray *comfy_modrinth_resolve_install (ComfyModrinthVersion *version,
                                         GHashTable        *installed,
                                         GPtrArray        **conflicts,
                                         GError           **error);

G_END_DECLS

#endif /* comfy_MODRINTH_H */
