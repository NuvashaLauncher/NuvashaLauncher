/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The Modrinth API v2, read with the download module's asynchronous
 * requests.
 *
 * The endpoints this uses, all of them GET unless said otherwise:
 *
 *   /search                       what a Browse page lists
 *   /project/<id>                 what a detail page shows
 *   /project/<id>/version         what a version list is read from,
 *                                 already narrowed by game_versions
 *                                 and loaders
 *   /version_file/<sha1>          which version a file on disk is
 *   /version_file/<sha1>/update   POST, whether that file has something
 *                                 newer that fits this instance
 *
 * What the server narrows is checked again here: a body only ever
 * becomes versions that fit the Minecraft version and the loader that
 * were asked for. That is what keeps an incompatible version out of
 * every list, and it is one rule in one place,
 * aml_modrinth_version_fits(), rather than a copy of it per caller.
 *
 * Every function is called from the thread-default main context that
 * answers it, normally the GTK main loop, and nothing here blocks it:
 * the requests are the download module's, and the only work done on the
 * main loop is parsing a body that has already arrived. */

#include "aml-modrinth.h"
#include "aml-task-private.h"

#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-i18n.h"
#include "aml-json.h"

#include <json-glib/json-glib.h>
#include <string.h>

GQuark
aml_modrinth_error_quark (void)
{
  return g_quark_from_static_string ("aml-modrinth-error");
}

char *
aml_modrinth_json_node_to_string (JsonNode *node)
{
  g_return_val_if_fail (node != NULL, NULL);
  return json_to_string (node, FALSE);
}

/* ---------------------------------------------------------- *
 * The kinds of content
 * ---------------------------------------------------------- */

const char *
aml_content_type_to_string (AmlContentType type)
{
  switch (type)
    {
    case AML_CONTENT_RESOURCE_PACK:
      return "resourcepack";
    case AML_CONTENT_SHADER_PACK:
      return "shader";
    case AML_CONTENT_MOD:
    default:
      return "mod";
    }
}

const char *
aml_content_type_title (AmlContentType type)
{
  switch (type)
    {
    case AML_CONTENT_RESOURCE_PACK:
      return _("Resource Packs");
    case AML_CONTENT_SHADER_PACK:
      return _("Shader Packs");
    case AML_CONTENT_MOD:
    default:
      return _("Mods");
    }
}

const char *
aml_content_type_folder (AmlContentType type)
{
  switch (type)
    {
    case AML_CONTENT_RESOURCE_PACK:
      return "resourcepacks";
    case AML_CONTENT_SHADER_PACK:
      return "shaderpacks";
    case AML_CONTENT_MOD:
    default:
      return "mods";
    }
}

gboolean
aml_content_type_needs_loader (AmlContentType type)
{
  return type == AML_CONTENT_MOD;
}

const char *
aml_content_type_extension (AmlContentType type)
{
  return aml_content_type_needs_loader (type) ? ".jar" : ".zip";
}

const char *
aml_content_type_icon (AmlContentType type)
{
  switch (type)
    {
    case AML_CONTENT_RESOURCE_PACK:
      return "image-x-generic-symbolic";
    case AML_CONTENT_SHADER_PACK:
      return "applications-graphics-symbolic";
    case AML_CONTENT_MOD:
    default:
      return "view-list-symbolic";
    }
}

/* ---------------------------------------------------------- *
 * Reading a JSON answer
 * ---------------------------------------------------------- */

static JsonNode *
root_node (const char *json, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) parse_error = NULL;
  JsonNode *root = NULL;

  if (!json_parser_load_from_data (parser, json != NULL ? json : "",
                                   json != NULL ? length : 0, &parse_error))
    {
      g_propagate_error (error, g_steal_pointer (&parse_error));
      return NULL;
    }

  root = json_parser_get_root (parser);

  if (root == NULL)
    {
      g_set_error_literal (error, AML_MODRINTH_ERROR,
                           AML_MODRINTH_ERROR_PARSE,
                           "the answer holds no JSON");
      return NULL;
    }

  /* The copy is what outlives the parser. */
  return json_node_copy (root);
}

/* The string @name of the object @node, or NULL. A field that is there
 * but is not a string counts as absent, so a malformed answer cannot
 * reach a list as an empty string where words were meant. */
static gboolean
string_array_has (GPtrArray *strings, const char *value)
{
  if (strings == NULL || value == NULL)
    return FALSE;

  for (guint i = 0; i < strings->len; i++)
    {
      if (g_strcmp0 (g_ptr_array_index (strings, i), value) == 0)
        return TRUE;
    }

  return FALSE;
}

/* One value as the JSON array Modrinth takes the narrowings as. */
static char *
string_array_json (const char *value)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_array (builder);
  json_builder_add_string_value (builder, value != NULL ? value : "");
  json_builder_end_array (builder);
  root = json_builder_get_root (builder);

  return json_to_string (root, FALSE);
}

/* TRUE when a version tagged @version_loader runs on an instance whose
 * loader is @instance_loader. Quilt runs Fabric mods, so a version that
 * only names Fabric is still a version of a Quilt instance. */
static gboolean
loader_runs (const char *instance_loader, const char *version_loader)
{
  if (g_strcmp0 (instance_loader, version_loader) == 0)
    return TRUE;

  return g_strcmp0 (instance_loader, "quilt") == 0 &&
         g_strcmp0 (version_loader, "fabric") == 0;
}

/* The loaders a request for @loader asks Modrinth for, as the JSON array
 * it takes: a Quilt instance asks for Fabric versions as well. */
static char *
loaders_json (const char *loader)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_array (builder);
  json_builder_add_string_value (builder, loader);

  if (g_strcmp0 (loader, "quilt") == 0)
    json_builder_add_string_value (builder, "fabric");

  json_builder_end_array (builder);
  root = json_builder_get_root (builder);

  return json_to_string (root, FALSE);
}

/* ---------------------------------------------------------- *
 * Projects
 * ---------------------------------------------------------- */

void
aml_modrinth_project_free (AmlModrinthProject *project)
{
  if (project == NULL)
    return;

  g_free (project->project_id);
  g_free (project->slug);
  g_free (project->title);
  g_free (project->description);
  g_free (project->author);
  g_free (project->icon_url);
  g_clear_pointer (&project->versions, g_ptr_array_unref);
  g_clear_pointer (&project->categories, g_ptr_array_unref);
  g_free (project);
}

/* One entry of a /search answer. Everything a result row shows is here,
 * so that listing a page of results costs no request per row. */
static AmlModrinthProject *
project_from_hit (JsonNode *hit, GError **error)
{
  AmlModrinthProject *project = g_new0 (AmlModrinthProject, 1);

  if (!aml_json_require_string (hit, "project_id", &project->project_id,
                                error) ||
      !aml_json_require_string (hit, "slug", &project->slug, error))
    {
      aml_modrinth_project_free (project);
      return NULL;
    }

  project->title = aml_json_string (hit, "title");
  project->description = aml_json_string (hit, "description");
  project->author = aml_json_string (hit, "author");
  project->icon_url = aml_json_string (hit, "icon_url");
  project->downloads = (guint64) aml_json_int (hit, "downloads", 0);
  project->followers = (guint64) aml_json_int (hit, "follows", 0);
  project->versions = aml_json_string_array (hit, "versions");
  project->categories = aml_json_string_array (hit, "categories");

  if (project->title == NULL)
    project->title = g_strdup (project->slug != NULL ? project->slug : "");

  if (project->description == NULL)
    project->description = g_strdup ("");

  if (project->author == NULL)
    project->author = g_strdup ("");

  if (project->project_id == NULL)
    project->project_id = g_strdup (project->slug != NULL ? project->slug : "");

  return project;
}

void
aml_modrinth_details_free (AmlModrinthDetails *details)
{
  if (details == NULL)
    return;

  g_free (details->project_id);
  g_free (details->slug);
  g_free (details->title);
  g_free (details->description);
  g_free (details->author);
  g_free (details->icon_url);
  g_free (details->body);
  g_free (details->license);
  g_free (details->source_url);
  g_free (details->issues_url);
  g_free (details->wiki_url);
  g_clear_pointer (&details->game_versions, g_ptr_array_unref);
  g_clear_pointer (&details->categories, g_ptr_array_unref);
  g_clear_pointer (&details->loaders, g_ptr_array_unref);
  g_free (details);
}

AmlModrinthDetails *
aml_modrinth_details_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonNode) root = root_node (json, length, error);
  JsonNode *team = NULL; /* borrowed from @root */
  AmlModrinthDetails *details = NULL;

  if (root == NULL)
    return NULL;

  details = g_new0 (AmlModrinthDetails, 1);
  details->project_id = aml_json_string (root, "id");
  details->slug = aml_json_string (root, "slug");
  details->title = aml_json_string (root, "title");
  details->description = aml_json_string (root, "description");
  details->body = aml_json_string (root, "body");
  details->icon_url = aml_json_string (root, "icon_url");
  details->source_url = aml_json_string (root, "source_url");
  details->issues_url = aml_json_string (root, "issues_url");
  details->wiki_url = aml_json_string (root, "wiki_url");
  details->downloads = (guint64) aml_json_int (root, "downloads", 0);
  details->followers = (guint64) aml_json_int (root, "followers", 0);
  details->game_versions = aml_json_string_array (root, "game_versions");
  details->categories = aml_json_string_array (root, "categories");
  details->loaders = aml_json_string_array (root, "loaders");

  /* The license is an object with an id, not a string. */
  details->license =
    aml_json_string (json_object_get_member (aml_json_object_of (root), "license"), "id");

  /* A search answer names an author; a project answer carries the team
   * instead, and Modrinth writes that as the team's project id rather
   * than as its members, so there is no name to take from it. Members
   * are read anyway in case that ever stops being true. */
  details->author = aml_json_string (root, "author");

  if (details->author == NULL)
    {
      team = json_object_get_member (aml_json_object_of (root), "team");

      if (team != NULL && JSON_NODE_HOLDS_ARRAY (team) &&
          json_array_get_length (json_node_get_array (team)) > 0)
        details->author = aml_json_string (
            json_array_get_element (json_node_get_array (team), 0), "name");
    }

  if (details->project_id == NULL)
    {
      g_set_error_literal (error, AML_MODRINTH_ERROR,
                           AML_MODRINTH_ERROR_PARSE,
                           "the project answer holds no id");
      aml_modrinth_details_free (details);
      return NULL;
    }

  if (details->title == NULL)
    details->title = g_strdup (details->project_id);

  if (details->description == NULL)
    details->description = g_strdup ("");

  if (details->body == NULL)
    details->body = g_strdup (details->description);

  if (details->author == NULL)
    details->author = g_strdup ("");

  return details;
}

/* ---------------------------------------------------------- *
 * Searches
 * ---------------------------------------------------------- */

void
aml_modrinth_search_free (AmlModrinthSearch *search)
{
  if (search == NULL)
    return;

  g_free (search->query);
  g_free (search->minecraft_version);
  g_free (search->loader);
  g_clear_pointer (&search->categories, g_ptr_array_unref);
  g_free (search);
}

void
aml_modrinth_results_free (AmlModrinthResults *results)
{
  if (results == NULL)
    return;

  g_clear_pointer (&results->projects, g_ptr_array_unref);
  g_free (results);
}

/* One facet: an array with "name:value" in it. Modrinth reads an AND
 * between the arrays and an OR inside one. */
static void
facet_add (GPtrArray *facets, const char *name, const char *value)
{
  g_autoptr (GPtrArray) inner = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *facet = g_strdup_printf ("%s:%s", name, value);

  g_ptr_array_add (inner, g_steal_pointer (&facet));
  g_ptr_array_add (facets, g_steal_pointer (&inner));
}

/* The loader facet: Quilt and Fabric in one array, because Modrinth reads
 * an OR inside it and Quilt runs Fabric mods. */
static void
facet_add_loaders (GPtrArray *facets, const char *loader)
{
  g_autoptr (GPtrArray) inner = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *facet = g_strdup_printf ("categories:%s", loader);

  g_ptr_array_add (inner, g_steal_pointer (&facet));

  if (g_strcmp0 (loader, "quilt") == 0)
    g_ptr_array_add (inner, g_strdup ("categories:fabric"));

  g_ptr_array_add (facets, g_steal_pointer (&inner));
}

char *
aml_modrinth_facets (const AmlModrinthSearch *search)
{
  g_autoptr (GPtrArray) facets =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_val_if_fail (search != NULL, g_strdup ("[[\"project_type:mod\"]]"));

  facet_add (facets, "project_type", aml_content_type_to_string (search->type));

  if (search->minecraft_version != NULL && *search->minecraft_version != '\0')
    facet_add (facets, "versions", search->minecraft_version);

  /* Only a mod is narrowed by loader: the game loads a resource pack
   * and a shader pack itself, whatever runs it. */
  if (aml_content_type_needs_loader (search->type) && search->loader != NULL &&
      *search->loader != '\0')
    facet_add_loaders (facets, search->loader);

  if (search->categories != NULL)
    {
      for (guint i = 0; i < search->categories->len; i++)
        {
          const char *category = g_ptr_array_index (search->categories, i);

          if (category != NULL && *category != '\0')
            facet_add (facets, "categories", category);
        }
    }

  /* The builder holds one root, so the outer array is opened once and
   * every facet goes into it. */
  json_builder_begin_array (builder);

  for (guint i = 0; i < facets->len; i++)
    {
      GPtrArray *inner = g_ptr_array_index (facets, i);

      json_builder_begin_array (builder);

      for (guint j = 0; j < inner->len; j++)
        json_builder_add_string_value (builder, g_ptr_array_index (inner, j));

      json_builder_end_array (builder);
    }

  json_builder_end_array (builder);

  root = json_builder_get_root (builder);

  return json_to_string (root, FALSE);
}

char *
aml_modrinth_search_url (const AmlModrinthSearch *search)
{
  g_autofree char *facets = NULL;
  g_autofree char *query = NULL;
  g_autofree char *escaped = NULL;

  g_return_val_if_fail (search != NULL, NULL);

  facets = aml_modrinth_facets (search);
  query = g_uri_escape_string (search->query != NULL ? search->query : "",
                              NULL, FALSE);
  escaped = g_uri_escape_string (facets, NULL, FALSE);

  return g_strdup_printf (
      AML_MODRINTH_URL "/search?query=%s&facets=%s&offset=%u&limit=%u", query,
      escaped, search->offset, search->limit > 0 ? search->limit : 20);
}

AmlModrinthResults *
aml_modrinth_results_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonNode) root = root_node (json, length, error);
  AmlModrinthResults *results = NULL;
  JsonNode *hits = NULL;

  if (root == NULL)
    return NULL;

  hits = json_object_get_member (aml_json_object_of (root), "hits");

  if (hits == NULL || !JSON_NODE_HOLDS_ARRAY (hits))
    {
      g_set_error_literal (error, AML_MODRINTH_ERROR,
                           AML_MODRINTH_ERROR_PARSE,
                           "the search answer holds no hits");
      return NULL;
    }

  results = g_new0 (AmlModrinthResults, 1);
  results->projects =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_modrinth_project_free);
  results->total_hits = (guint64) aml_json_int (root, "total_hits", 0);

  {
    JsonArray *array = json_node_get_array (hits);
    guint length = json_array_get_length (array);

    for (guint i = 0; i < length; i++)
      {
        JsonNode *hit = json_array_get_element (array, i);

        /* A hit that is not an object has no title and no id, so there
         * is nothing a row could show for it. */
        if (aml_json_object_of (hit) == NULL)
          continue;

        {
          g_autoptr (GError) result_error = NULL;
          AmlModrinthProject *project = project_from_hit (hit, &result_error);

          if (project == NULL)
            {
              g_set_error (error, AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_PARSE,
                           "invalid search result: %s",
                           result_error != NULL ? result_error->message
                                                 : "invalid project");
              aml_modrinth_results_free (results);
              return NULL;
            }

          g_ptr_array_add (results->projects, project);
         }
      }
  }

  return results;
}

/* ---------------------------------------------------------- *
 * Versions
 * ---------------------------------------------------------- */

const char *
aml_modrinth_dependency_to_string (AmlModrinthDependencyType type)
{
  switch (type)
    {
    case AML_DEPENDENCY_OPTIONAL:
      return "optional";
    case AML_DEPENDENCY_EMBEDDED:
      return "embedded";
    case AML_DEPENDENCY_INCOMPATIBLE:
      return "incompatible";
    case AML_DEPENDENCY_REQUIRED:
    default:
      return "required";
    }
}

/* Modrinth writes nothing for "required", which is the kind an install
 * has to fetch, so a dependency without the field is that one. */
static AmlModrinthDependencyType
dependency_type_from_string (const char *value)
{
  if (g_strcmp0 (value, "optional") == 0)
    return AML_DEPENDENCY_OPTIONAL;
  if (g_strcmp0 (value, "embedded") == 0)
    return AML_DEPENDENCY_EMBEDDED;
  if (g_strcmp0 (value, "incompatible") == 0)
    return AML_DEPENDENCY_INCOMPATIBLE;

  return AML_DEPENDENCY_REQUIRED;
}

void
aml_modrinth_file_free (AmlModrinthFile *file)
{
  if (file == NULL)
    return;

  g_free (file->filename);
  g_free (file->url);
  g_free (file->sha1);
  g_free (file->sha512);
  g_free (file);
}

void
aml_modrinth_dependency_free (AmlModrinthDependency *dependency)
{
  if (dependency == NULL)
    return;

  g_free (dependency->project_id);
  g_free (dependency->version_id);
  g_free (dependency->file_name);
  g_free (dependency);
}

/* A copy of a dependency, so a caller can hold it without keeping the
 * version it came from alive. (transfer full) */
AmlModrinthDependency *
aml_modrinth_dependency_copy (AmlModrinthDependency *dependency)
{
  AmlModrinthDependency *copy = NULL;

  if (dependency == NULL)
    return NULL;

  copy = g_new0 (AmlModrinthDependency, 1);
  copy->type = dependency->type;
  copy->project_id = g_strdup (dependency->project_id);
  copy->version_id = g_strdup (dependency->version_id);
  copy->file_name = g_strdup (dependency->file_name);

  return copy;
}

AmlModrinthProject *
aml_modrinth_project_copy (AmlModrinthProject *project)
{
  AmlModrinthProject *copy = NULL;

  if (project == NULL)
    return NULL;

  copy = g_new0 (AmlModrinthProject, 1);
  copy->project_id = g_strdup (project->project_id);
  copy->slug = g_strdup (project->slug);
  copy->title = g_strdup (project->title);
  copy->description = g_strdup (project->description);
  copy->author = g_strdup (project->author);
  copy->icon_url = g_strdup (project->icon_url);
  copy->downloads = project->downloads;
  copy->followers = project->followers;

  copy->versions = g_ptr_array_new_with_free_func (g_free);

  if (project->versions != NULL)
    {
      for (guint i = 0; i < project->versions->len; i++)
        g_ptr_array_add (
            copy->versions,
            g_strdup (g_ptr_array_index (project->versions, i)));
    }

  copy->categories = g_ptr_array_new_with_free_func (g_free);

  if (project->categories != NULL)
    {
      for (guint i = 0; i < project->categories->len; i++)
        g_ptr_array_add (
            copy->categories,
            g_strdup (g_ptr_array_index (project->categories, i)));
    }

  return copy;
}

void
aml_modrinth_version_free (AmlModrinthVersion *version)
{
  if (version == NULL)
    return;

  g_free (version->version_id);
  g_free (version->project_id);
  g_free (version->name);
  g_free (version->version_number);
  g_free (version->changelog);
  g_free (version->date_published);
  g_clear_pointer (&version->game_versions, g_ptr_array_unref);
  g_clear_pointer (&version->loaders, g_ptr_array_unref);
  g_clear_pointer (&version->files, g_ptr_array_unref);
  g_clear_pointer (&version->dependencies, g_ptr_array_unref);
  g_free (version);
}

AmlModrinthVersion *
aml_modrinth_version_copy (AmlModrinthVersion *version)
{
  AmlModrinthVersion *copy = NULL;

  if (version == NULL)
    return NULL;

  copy = g_new0 (AmlModrinthVersion, 1);
  copy->version_id = g_strdup (version->version_id);
  copy->project_id = g_strdup (version->project_id);
  copy->name = g_strdup (version->name);
  copy->version_number = g_strdup (version->version_number);
  copy->changelog = g_strdup (version->changelog);
  copy->date_published = g_strdup (version->date_published);
  copy->downloads = version->downloads;
  copy->game_versions = g_ptr_array_new_with_free_func (g_free);
  copy->loaders = g_ptr_array_new_with_free_func (g_free);
  copy->files = g_ptr_array_new_with_free_func (
      (GDestroyNotify) aml_modrinth_file_free);
  copy->dependencies = g_ptr_array_new_with_free_func (
      (GDestroyNotify) aml_modrinth_dependency_free);

  for (guint i = 0; i < version->game_versions->len; i++)
    g_ptr_array_add (copy->game_versions,
                     g_strdup (g_ptr_array_index (version->game_versions, i)));

  for (guint i = 0; i < version->loaders->len; i++)
    g_ptr_array_add (copy->loaders,
                     g_strdup (g_ptr_array_index (version->loaders, i)));

  for (guint i = 0; i < version->files->len; i++)
    {
      AmlModrinthFile *file = g_ptr_array_index (version->files, i);
      AmlModrinthFile *file_copy = g_new0 (AmlModrinthFile, 1);

      file_copy->filename = g_strdup (file->filename);
      file_copy->url = g_strdup (file->url);
      file_copy->sha1 = g_strdup (file->sha1);
      file_copy->sha512 = g_strdup (file->sha512);
      file_copy->size = file->size;
      file_copy->primary = file->primary;
      g_ptr_array_add (copy->files, file_copy);
    }

  for (guint i = 0; i < version->dependencies->len; i++)
    g_ptr_array_add (
        copy->dependencies,
        aml_modrinth_dependency_copy (
            g_ptr_array_index (version->dependencies, i)));

  return copy;
}

AmlModrinthFile *
aml_modrinth_version_primary_file (AmlModrinthVersion *version)
{
  AmlModrinthFile *first = NULL;

  if (version == NULL || version->files == NULL)
    return NULL;

  for (guint i = 0; i < version->files->len; i++)
    {
      AmlModrinthFile *file = g_ptr_array_index (version->files, i);

      if (first == NULL)
        first = file;

      if (file->primary)
        return file;
    }

  return first;
}

gboolean
aml_modrinth_version_has_hash (const AmlModrinthVersion *version,
                               const char               *sha1)
{
  g_return_val_if_fail (version != NULL, FALSE);

  if (sha1 == NULL || *sha1 == '\0' || version->files == NULL)
    return FALSE;

  for (guint i = 0; i < version->files->len; i++)
    {
      const AmlModrinthFile *file = g_ptr_array_index (version->files, i);

      if (file->sha1 != NULL && g_ascii_strcasecmp (file->sha1, sha1) == 0)
        return TRUE;
    }

  return FALSE;
}

/* One file of a version answer, or NULL when it names no file and no
 * url, which is nothing to download. */
static AmlModrinthFile *
file_from_node (JsonNode *node)
{
  g_autofree char *filename = aml_json_string (node, "filename");
  g_autofree char *url = aml_json_string (node, "url");
  g_autofree char *sha1 = NULL;
  g_autofree char *sha512 = NULL;
  AmlModrinthFile *file = NULL;

  if (filename == NULL || url == NULL)
    return NULL;

  {
    JsonNode *hashes = json_object_get_member (aml_json_object_of (node), "hashes");

    if (hashes != NULL)
      {
        sha1 = aml_json_string (hashes, "sha1");
        sha512 = aml_json_string (hashes, "sha512");
      }
  }

  file = g_new0 (AmlModrinthFile, 1);
  file->filename = g_steal_pointer (&filename);
  file->url = g_steal_pointer (&url);
  file->sha1 = g_steal_pointer (&sha1);
  file->sha512 = g_steal_pointer (&sha512);
  file->primary = aml_json_boolean (node, "primary", FALSE);
  file->size = (goffset) aml_json_int (node, "size", 0);

  return file;
}

static AmlModrinthDependency *
dependency_from_node (JsonNode *node)
{
  g_autofree char *type = aml_json_string (node, "dependency_type");
  AmlModrinthDependency *dependency = g_new0 (AmlModrinthDependency, 1);

  dependency->type = dependency_type_from_string (type);
  dependency->project_id = aml_json_string (node, "project_id");
  dependency->version_id = aml_json_string (node, "version_id");
  dependency->file_name = aml_json_string (node, "file_name");

  return dependency;
}

/* One version answer, or NULL when there is nothing installable in it:
 * without an id it cannot be asked about again, and without a file
 * there is nothing to download. */
static AmlModrinthVersion *
version_from_node (JsonNode *node)
{
  AmlModrinthVersion *version = NULL;
  JsonNode *array_node = NULL;

  if (aml_json_object_of (node) == NULL)
    return NULL;

  version = g_new0 (AmlModrinthVersion, 1);
  version->version_id = aml_json_string (node, "id");
  version->project_id = aml_json_string (node, "project_id");
  version->name = aml_json_string (node, "name");
  version->version_number = aml_json_string (node, "version_number");
  version->changelog = aml_json_string (node, "changelog");
  version->date_published = aml_json_string (node, "date_published");
  version->downloads = (guint64) aml_json_int (node, "downloads", 0);
  version->game_versions = aml_json_string_array (node, "game_versions");
  version->loaders = aml_json_string_array (node, "loaders");

  version->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_modrinth_file_free);
  array_node = json_object_get_member (aml_json_object_of (node), "files");

  if (array_node != NULL && JSON_NODE_HOLDS_ARRAY (array_node))
    {
      JsonArray *array = json_node_get_array (array_node);
      guint length = json_array_get_length (array);

      for (guint i = 0; i < length; i++)
        {
          AmlModrinthFile *file =
            file_from_node (json_array_get_element (array, i));

          if (file != NULL)
            g_ptr_array_add (version->files, file);
        }
    }

  version->dependencies = g_ptr_array_new_with_free_func (
      (GDestroyNotify) aml_modrinth_dependency_free);
  array_node = json_object_get_member (aml_json_object_of (node), "dependencies");

  if (array_node != NULL && JSON_NODE_HOLDS_ARRAY (array_node))
    {
      JsonArray *array = json_node_get_array (array_node);
      guint length = json_array_get_length (array);

      for (guint i = 0; i < length; i++)
        {
          JsonNode *item = json_array_get_element (array, i);

          if (aml_json_object_of (item) != NULL)
            g_ptr_array_add (version->dependencies, dependency_from_node (item));
        }
    }

  if (version->version_id == NULL || version->files->len == 0)
    {
      aml_modrinth_version_free (version);
      return NULL;
    }

  if (version->project_id == NULL)
    version->project_id = g_strdup ("");

  if (version->name == NULL)
    version->name = g_strdup (version->version_id);

  if (version->version_number == NULL)
    version->version_number = g_strdup ("");

  if (version->changelog == NULL)
    version->changelog = g_strdup ("");

  if (version->date_published == NULL)
    version->date_published = g_strdup ("");

  return version;
}

AmlModrinthVersion *
aml_modrinth_version_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonNode) root = root_node (json, length, error);
  AmlModrinthVersion *version = NULL;

  if (root == NULL)
    return NULL;

  version = version_from_node (root);

  if (version == NULL)
    {
      g_set_error_literal (error, AML_MODRINTH_ERROR,
                           AML_MODRINTH_ERROR_PARSE,
                           "the answer is not a version with a file");
      return NULL;
    }

  return version;
}

/* The one rule of compatibility.
 *
 * A mod runs on one Minecraft version and one loader; a resource pack
 * and a shader pack are loaded by the game itself, so the Minecraft
 * version is all that decides them. FALSE is the answer that keeps an
 * incompatible version out of every list. */
gboolean
aml_modrinth_version_fits (AmlModrinthVersion *version, AmlContentType type,
                          const char *minecraft_version, const char *loader)
{
  if (version == NULL || minecraft_version == NULL ||
      *minecraft_version == '\0')
    return FALSE;

  if (!string_array_has (version->game_versions, minecraft_version))
    return FALSE;

  if (!aml_content_type_needs_loader (type) || loader == NULL ||
      *loader == '\0')
    return TRUE;

  /* A mod that names no loader is one the loader runs itself, so it is
   * nothing to offer for an instance that has one. */
  if (version->loaders == NULL || version->loaders->len == 0)
    return FALSE;

  for (guint i = 0; i < version->loaders->len; i++)
    {
      if (loader_runs (loader, g_ptr_array_index (version->loaders, i)))
        return TRUE;
    }

  return FALSE;
}

/* Newest first, which is the order a list of versions is read in. The
 * dates are ISO 8601 in UTC, so the order of the strings is the order
 * of the moments they name. */
static int
compare_by_date (gconstpointer a, gconstpointer b)
{
  const AmlModrinthVersion *left = *(const AmlModrinthVersion *const *) a;
  const AmlModrinthVersion *right = *(const AmlModrinthVersion *const *) b;

  return strcmp (right->date_published, left->date_published);
}

GPtrArray *
aml_modrinth_versions_from_json (const char *json, gssize length,
                                 AmlContentType type,
                                 const char *minecraft_version,
                                 const char *loader, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) parse_error = NULL;
  GPtrArray *versions = NULL;
  JsonNode *root = NULL;
  JsonArray *array = NULL;
  guint count = 0;
  guint compatible = 0;

  if (!json_parser_load_from_data (parser, json != NULL ? json : "",
                                   json != NULL ? length : 0, &parse_error))
    {
      g_propagate_error (error, g_steal_pointer (&parse_error));
      return NULL;
    }

  root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error, AML_MODRINTH_ERROR,
                           AML_MODRINTH_ERROR_PARSE,
                           "the versions answer is not a list");
      return NULL;
    }

  versions = g_ptr_array_new_with_free_func (
      (GDestroyNotify) aml_modrinth_version_free);
  array = json_node_get_array (root);
  count = json_array_get_length (array);

  for (guint i = 0; i < count; i++)
    {
      AmlModrinthVersion *version =
        version_from_node (json_array_get_element (array, i));

      if (version == NULL)
        continue;

      /* The server was already asked for these, so what is left here is
       * the check that keeps an incompatible version out of the list. */
      if (!aml_modrinth_version_fits (version, type, minecraft_version,
                                      loader))
        {
          aml_modrinth_version_free (version);
          continue;
        }

      g_ptr_array_add (versions, version);
      compatible++;
    }

  if (compatible == 0)
    {
      g_autofree char *message = aml_modrinth_no_compatible_message (
          type, minecraft_version, loader);

      g_ptr_array_unref (versions);
      g_set_error (error, AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_NOT_FOUND,
                   "%s", message);
      return NULL;
    }

  g_ptr_array_sort (versions, compare_by_date);

  return versions;
}

/* ---------------------------------------------------------- *
 * The requests
 * ---------------------------------------------------------- */

/* What a request hands its parsed answer to. */
typedef void (*AnswerFunc) (GTask *task, JsonNode *root);

/* What an HTTP 404 means to a request: for an update check it is the
 * answer that nothing is newer, for a hash lookup it says the file is
 * not one Modrinth published. */
typedef enum {
  AML_NOT_FOUND_IS_ERROR, /* the answer came from a known place */
  AML_NOT_FOUND_IS_ABSENT /* nothing there is an answer, not a failure */
} AmlNotFoundRule;

typedef struct {
  AnswerFunc answer;
  AmlNotFoundRule not_found;
  AmlTaskScope *scope;
  void *data;
  GDestroyNotify free_data;
} Request;

typedef struct {
  char *key;
  GPtrArray *tasks; /* GTask*, one for each caller */
} JsonFlight;

static GHashTable *json_flights;

static void
json_flight_free (JsonFlight *flight)
{
  g_free (flight->key);
  g_clear_pointer (&flight->tasks, g_ptr_array_unref);
  g_free (flight);
}

static void
request_free (gpointer data)
{
  Request *request = data;

  if (request->data != NULL && request->free_data != NULL)
    request->free_data (request->data);

  g_free (request);
}

static void
request_return_error (GTask *task, GError *error)
{
  Request *request = g_task_get_task_data (task);
  aml_task_scope_return_error (request->scope, error);
}

static void
request_return_pointer (GTask *task, gpointer value, GDestroyNotify free_value)
{
  Request *request = g_task_get_task_data (task);
  aml_task_scope_return_pointer (request->scope, value, free_value);
}

static AmlTaskScope *
request_new (GAsyncReadyCallback callback, gpointer user_data,
             AnswerFunc answer, AmlNotFoundRule not_found, void *data,
             GDestroyNotify free_data)
{
  AmlTaskScope *scope = aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  Request *request = g_new0 (Request, 1);

  /* Borrowed: this request is the task's own data and the scope owns that
     task. The caller keeps the operation alive until its callback. */
  request->scope = scope;

  request->answer = answer;
  request->not_found = not_found;
  request->data = data;
  request->free_data = free_data;

  /* Cancellation is reported in this module's own domain, so GTask must
   * not replace the error with G_IO_ERROR_CANCELLED. */
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_task_data (task, request, request_free);

  return scope;
}

/* One way out of a request: an error of this module's domain, so that a
 * caller can tell an answer AML cannot use from a network that is not
 * there. What the download module said is kept, since it names the url
 * that failed. */
static void
fail (GTask *task, GError *error)
{
  Request *request = g_task_get_task_data (task);
  GError *wrapped = NULL;

  /* The cancellation is reported once, from here. The task must not be
     answered by g_task_return_error_if_cancelled() first and by the
     error below second: a GTask answers only once. */
  if (g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
      g_clear_error (&error);

      aml_task_scope_return_error (
        request->scope,
        g_error_new_literal (AML_MODRINTH_ERROR,
                             AML_MODRINTH_ERROR_NETWORK,
                             "the request was cancelled"));
      return;
    }

  if (error->domain == AML_DOWNLOAD_ERROR)
    {
      if (error->code == AML_DOWNLOAD_ERROR_RATE_LIMITED)
        wrapped = g_error_new_literal (
            AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_RATE_LIMIT,
            "Modrinth is asking AML to slow down, too many requests");
      else if (error->code == AML_DOWNLOAD_ERROR_HTTP)
        wrapped = g_error_new (AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_HTTP,
                               "%s", error->message);
      else
        wrapped = g_error_new (AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_NETWORK,
                               "%s", error->message);
    }
  else
    wrapped = g_error_new (AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_PARSE,
                           "%s", error->message);

  g_error_free (error);
  aml_task_scope_return_error (request->scope, wrapped);
}

/* The download module writes the status into its own message, which is
 * the one place the code of an HTTP error lives. */
static gboolean
error_is_not_found (const GError *error)
{
  return error->domain == AML_DOWNLOAD_ERROR &&
         error->code == AML_DOWNLOAD_ERROR_HTTP &&
         strstr (error->message, "HTTP 404") != NULL;
}

static void
json_flight_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  JsonFlight *flight = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = aml_download_json_finish (result, &error);
  JsonNode *root = parser != NULL ? json_parser_get_root (parser) : NULL;

  /* Remove before delivering callbacks: a callback may immediately request
   * the same URL again, and that request must start a fresh flight. */
  g_hash_table_steal (json_flights, flight->key);

  for (guint i = 0; i < flight->tasks->len; i++)
    {
      GTask *task = g_ptr_array_index (flight->tasks, i);
      Request *request = g_task_get_task_data (task);

      if (aml_task_scope_return_error_if_cancelled (request->scope))
        continue;

      if (parser == NULL)
        {
          if (error_is_not_found (error) &&
              request->not_found == AML_NOT_FOUND_IS_ABSENT)
            {
               aml_task_scope_return_pointer (request->scope, NULL, NULL);
              continue;
            }

          if (error_is_not_found (error))
            {
               aml_task_scope_return_error (
                 request->scope,
                 g_error_new_literal (AML_MODRINTH_ERROR,
                                      AML_MODRINTH_ERROR_NOT_FOUND,
                                      "Modrinth does not know this file"));
              continue;
            }

          fail (task, g_error_copy (error));
          continue;
        }

      if (root == NULL)
        {
           aml_task_scope_return_error (
             request->scope,
             g_error_new_literal (AML_MODRINTH_ERROR,
                                  AML_MODRINTH_ERROR_PARSE,
                                  "the answer holds no JSON"));
          continue;
        }

      {
        g_autoptr (JsonNode) answer = json_node_copy (root);
        request->answer (task, answer);
      }
    }

  json_flight_free (flight);
}

static void
json_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  /* The flight stores this task as callback data without owning another
     reference. Keep one while this callback uses the task, but do not treat
     the borrowed callback pointer itself as an owned reference. */
  g_autoptr (GTask) task = g_object_ref (user_data);
  Request *request = g_task_get_task_data (task);
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) error = NULL;
  JsonNode *root = NULL;

  parser = aml_download_json_finish (result, &error);

  if (parser == NULL)
    {
      if (error_is_not_found (error) &&
          request->not_found == AML_NOT_FOUND_IS_ABSENT)
        {
          g_clear_error (&error);
          request_return_pointer (task, NULL, NULL);
          return;
        }

      if (error_is_not_found (error))
        {
          g_clear_error (&error);
          request_return_error (
            task,
            g_error_new_literal (AML_MODRINTH_ERROR,
                                 AML_MODRINTH_ERROR_NOT_FOUND,
                                 "Modrinth does not know this file"));
          return;
        }

      fail (task, g_steal_pointer (&error));
      return;
    }

  root = json_parser_get_root (parser);

  if (root == NULL)
    {
      request_return_error (
        task,
        g_error_new_literal (AML_MODRINTH_ERROR, AML_MODRINTH_ERROR_PARSE,
                             "the answer holds no JSON"));
      return;
    }

  /* The node is copied out of the parser, so nothing of it outlives
   * this call. */
  {
    g_autoptr (JsonNode) answer = json_node_copy (root);
    request->answer (task, answer);
  }
}

static AmlTaskScope *
get_async (const char *url, GAsyncReadyCallback callback, gpointer user_data,
           AnswerFunc answer, AmlNotFoundRule not_found)
{
  g_autoptr (AmlTaskScope) scope =
    request_new (callback, user_data, answer, not_found, NULL, NULL);
  GTask *task = aml_task_scope_task (scope);
  JsonFlight *flight = NULL;

  if (json_flights == NULL)
    json_flights = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify) json_flight_free);

  flight = g_hash_table_lookup (json_flights, url);
  if (flight != NULL)
    {
      g_ptr_array_add (flight->tasks, g_object_ref (task));
      return g_steal_pointer (&scope);
    }

  flight = g_new0 (JsonFlight, 1);
  flight->key = g_strdup (url);
  flight->tasks = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (flight->tasks, g_object_ref (task));
  g_hash_table_insert (json_flights, g_strdup (url), flight);

  aml_task_scope_release_when_done (
    aml_download_json_cached_async (aml_downloads_get_service (), url, json_flight_done, flight),
    aml_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

/* ---------------------------------------------------------- *
 * Searches
 * ---------------------------------------------------------- */

static void
search_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  AmlModrinthResults *results = NULL;

  results = aml_modrinth_results_from_json (json, -1, &error);

  if (results == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, results,
                         (GDestroyNotify) aml_modrinth_results_free);
}

AmlTaskScope *
aml_modrinth_search_async (const AmlModrinthSearch *search,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (search != NULL, NULL);

  url = aml_modrinth_search_url (search);
  return get_async (url, callback, user_data, search_answer,
                    AML_NOT_FOUND_IS_ERROR);
}

AmlModrinthResults *
aml_modrinth_search_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
details_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  AmlModrinthDetails *details = NULL;

  details = aml_modrinth_details_from_json (json, -1, &error);

  if (details == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, details,
                         (GDestroyNotify) aml_modrinth_details_free);
}

AmlTaskScope *
aml_modrinth_details_async (const char *project_id, GAsyncReadyCallback callback,
                            gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);

  {
    g_autofree char *escaped_id =
      g_uri_escape_string (project_id, NULL, FALSE);

    url = g_strdup_printf (AML_MODRINTH_URL "/project/%s", escaped_id);
  }
  return get_async (url, callback, user_data, details_answer,
                    AML_NOT_FOUND_IS_ERROR);
}
AmlModrinthDetails *
aml_modrinth_details_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The versions of a project
 * ---------------------------------------------------------- */

/* What a request has to remember about the instance it is asked for, so
 * that the answer is checked against the same one. */
typedef struct {
  AmlContentType type;
  char *minecraft_version;
  char *loader;
} InstanceAsk;

static void
instance_ask_free (gpointer data)
{
  InstanceAsk *ask = data;

  g_free (ask->minecraft_version);
  g_free (ask->loader);
  g_free (ask);
}

static InstanceAsk *
instance_ask_new (AmlContentType type, const char *minecraft_version,
                  const char *loader)
{
  InstanceAsk *ask = g_new0 (InstanceAsk, 1);

  ask->type = type;
  ask->minecraft_version = g_strdup (minecraft_version);
  ask->loader = loader != NULL ? g_strdup (loader) : NULL;

  return ask;
}

static void
versions_answer (GTask *task, JsonNode *root)
{
  Request *request = g_task_get_task_data (task);
  InstanceAsk *asked = request->data;
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  GPtrArray *versions = NULL;

  versions = aml_modrinth_versions_from_json (json, -1, asked->type,
                                              asked->minecraft_version,
                                              asked->loader, &error);

  if (versions == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, versions, (GDestroyNotify) g_ptr_array_unref);
}

AmlTaskScope *
aml_modrinth_versions_async (const char *project_id, AmlContentType type,
                             const char *minecraft_version, const char *loader,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *escaped_id = NULL;
  g_autofree char *escaped_game = NULL;
  g_autofree char *escaped_loaders = NULL;
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  game_versions = string_array_json (minecraft_version);
  escaped_id = g_uri_escape_string (project_id, NULL, FALSE);
  escaped_game = g_uri_escape_string (game_versions, NULL, FALSE);

  /* Only a mod is asked for by loader.
   *
   * A resource pack and a shader pack are not filtered by it at all, and
   * they must not be: a shader version publishes the loaders that can
   * read it, which are "iris", "optifine" and such, and no shader version
   * publishes "minecraft". Asking Modrinth for a shader with
   * loaders=minecraft answers with an empty list, which is how a shader
   * that plainly supports the Minecraft version came to be reported as
   * having no compatible version. */
  if (aml_content_type_needs_loader (type) && loader != NULL &&
      *loader != '\0')
    {
      loaders = loaders_json (loader);
      escaped_loaders = g_uri_escape_string (loaders, NULL, FALSE);
    }

  if (escaped_loaders != NULL)
    url = g_strdup_printf (
        AML_MODRINTH_URL "/project/%s/version?game_versions=%s&loaders=%s",
        escaped_id, escaped_game, escaped_loaders);
  else
    url = g_strdup_printf (
        AML_MODRINTH_URL "/project/%s/version?game_versions=%s",
        escaped_id, escaped_game);

  scope = request_new (callback, user_data, versions_answer,
                       AML_NOT_FOUND_IS_ERROR,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  aml_task_scope_release_when_done (
    aml_download_json_cached_async (aml_downloads_get_service (), url, json_ready,
                                    aml_task_scope_task (scope)),
    aml_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
aml_modrinth_versions_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The categories a search can be narrowed by
 * ---------------------------------------------------------- */

void
aml_modrinth_category_free (AmlModrinthCategory *category)
{
  if (category == NULL)
    return;

  g_free (category->name);
  g_free (category->header);
  g_free (category);
}

/* "colored-lighting" is what a facet is built from and "Colored Lighting"
 * is what a person reads; the two are kept apart so the slug is never
 * changed on the way out. */
char *
aml_modrinth_category_label (const char *name)
{
  g_autofree char *spaced = NULL;

  if (name == NULL || *name == '\0')
    return g_strdup ("");

  spaced = g_strdelimit (g_strdup (name), "-_", ' ');

  /* Title-case each word: the first letter after a space goes upper
     case, the rest keep their shape, so "512x+" stays "512x+". */
  {
    gboolean word_start = TRUE;

    for (char *p = spaced; *p != '\0'; p++)
      {
        if (*p == ' ')
          {
            word_start = TRUE;
            continue;
          }

        if (word_start && g_ascii_isalpha (*p))
          *p = g_ascii_toupper (*p);

        word_start = FALSE;
      }
  }

  return g_steal_pointer (&spaced);
}

char *
aml_modrinth_project_type_for_content (AmlContentType type)
{
  return g_strdup (aml_content_type_to_string (type));
}

static int
compare_by_name (gconstpointer a, gconstpointer b)
{
  const AmlModrinthCategory *left = *(AmlModrinthCategory *const *) a;
  const AmlModrinthCategory *right = *(AmlModrinthCategory *const *) b;

  return g_strcmp0 (left->name, right->name);
}

GPtrArray *
aml_modrinth_categories_from_json (const char *json, gssize length,
                                   const char *project_type, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) parse_error = NULL;
  GPtrArray *categories = NULL;
  JsonNode *root = NULL;
  JsonArray *array = NULL;

  if (!json_parser_load_from_data (parser, json != NULL ? json : "",
                                   json != NULL ? length : 0, &parse_error))
    {
      g_propagate_error (error, g_steal_pointer (&parse_error));
      return NULL;
    }

  root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error, AML_MODRINTH_ERROR,
                           AML_MODRINTH_ERROR_PARSE,
                           "the categories answer is not a list");
      return NULL;
    }

  categories =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_modrinth_category_free);
  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      g_autofree char *item_type = NULL;
      g_autofree char *name = NULL;
      g_autofree char *header = NULL;
      AmlModrinthCategory *category = NULL;
      JsonNode *item = json_array_get_element (array, i);

      if (aml_json_object_of (item) == NULL)
        continue;

      /* One project type publishes one list of categories, so the other
       * kinds' categories are not offered here. This is also what keeps a
       * pack page from offering "fabric". */
      item_type = aml_json_string (item, "project_type");
      name = aml_json_string (item, "name");

      if (name == NULL || g_strcmp0 (item_type, project_type) != 0)
        continue;

      header = aml_json_string (item, "header");

      category = g_new0 (AmlModrinthCategory, 1);
      category->name = g_steal_pointer (&name);
      category->header = g_steal_pointer (&header);
      g_ptr_array_add (categories, category);
    }

  g_ptr_array_sort (categories, compare_by_name);

  return categories;
}

static void
categories_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  Request *request = g_task_get_task_data (task);
  GPtrArray *categories = NULL;

  categories = aml_modrinth_categories_from_json (
      json, -1, (const char *) request->data, &error);

  if (categories == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, categories, (GDestroyNotify) g_ptr_array_unref);
}

AmlTaskScope *
aml_modrinth_categories_async (const char        *project_type,
                               GAsyncReadyCallback callback,
                               gpointer           user_data)
{
  g_autofree char *url = g_strdup_printf (AML_MODRINTH_URL "/tag/category");
  g_autoptr (AmlTaskScope) scope = NULL;

  scope = request_new (callback, user_data, categories_answer,
                       AML_NOT_FOUND_IS_ERROR, g_strdup (project_type), g_free);
  aml_task_scope_release_when_done (
    aml_download_json_cached_async (aml_downloads_get_service (), url, json_ready,
                                    aml_task_scope_task (scope)),
    aml_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
aml_modrinth_categories_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * One version out of the hash of a file
 * ---------------------------------------------------------- */

static void
version_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  AmlModrinthVersion *version = NULL;

  version = aml_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, version,
                         (GDestroyNotify) aml_modrinth_version_free);
}

AmlTaskScope *
aml_modrinth_version_by_hash_async (const char *sha1,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (AML_MODRINTH_URL "/version_file/%s?algorithm=sha1",
                           escaped_sha1);
  }
  return get_async (url, callback, user_data, version_answer,
                    AML_NOT_FOUND_IS_ERROR);
}

AmlModrinthVersion *
aml_modrinth_version_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

AmlTaskScope *
aml_modrinth_version_by_id_async (const char *version_id,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  g_autofree char *escaped = g_uri_escape_string (version_id, NULL, FALSE);
  g_autofree char *url = g_strdup_printf (AML_MODRINTH_URL "/version/%s",
                                          escaped);

  return get_async (url, callback, user_data, version_answer,
                    AML_NOT_FOUND_IS_ERROR);
}

/* ---------------------------------------------------------- *
 * An update out of the hash of a file
 * ---------------------------------------------------------- */

static void
update_answer (GTask *task, JsonNode *root)
{
  Request *request = g_task_get_task_data (task);
  InstanceAsk *asked = request->data;
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  AmlModrinthVersion *version = NULL;

  version = aml_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* Modrinth was asked for something that fits this instance, so
   * anything else it answered is not an update this instance can take. */
  if (!aml_modrinth_version_fits (version, asked->type,
                                  asked->minecraft_version, asked->loader))
    {
      aml_modrinth_version_free (version);
      request_return_pointer (task, NULL, NULL);
      return;
    }

  request_return_pointer (task, version,
                         (GDestroyNotify) aml_modrinth_version_free);
}

AmlTaskScope *
aml_modrinth_update_by_hash_async (AmlContentType type, const char *sha1,
                                   const char *minecraft_version,
                                   const char *loader,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope = NULL;
  g_autofree char *url = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *body = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  /* Both narrowings are already JSON arrays, so the body is written
   * around them rather than through a builder. */
  game_versions = string_array_json (minecraft_version);
  loaders = string_array_json (aml_content_type_needs_loader (type) &&
                                       loader != NULL
                                   ? loader
                                   : "minecraft");

  body = g_strdup_printf ("{\"game_versions\":%s,\"loaders\":%s}",
                          game_versions, loaders);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (
        AML_MODRINTH_URL "/version_file/%s/update?algorithm=sha1",
        escaped_sha1);
  }

  scope = request_new (callback, user_data, update_answer,
                       AML_NOT_FOUND_IS_ABSENT,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  aml_task_scope_release_when_done (
    aml_download_json_post_cached_async (aml_downloads_get_service (), url, body, AML_MODRINTH_USER_AGENT,
                                         json_ready,
                                         aml_task_scope_task (scope)),
    aml_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

AmlModrinthVersion *
aml_modrinth_update_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * What an install of a version needs
 * ---------------------------------------------------------- */

char *
aml_modrinth_no_compatible_message (AmlContentType type,
                                    const char *minecraft_version,
                                    const char *loader)
{
  const char *version = minecraft_version != NULL ? minecraft_version : "";

  if (aml_content_type_needs_loader (type))
    return g_strdup_printf (_("No compatible version found for Minecraft %s and %s."),
                            version, loader != NULL ? loader : "");

  return g_strdup_printf (_("No compatible version found for Minecraft %s."),
                          version);
}

GPtrArray *
aml_modrinth_resolve_install (AmlModrinthVersion *version,
                              GHashTable *installed, GPtrArray **conflicts,
                              GError **error)
{
  GPtrArray *pending = NULL;

  g_return_val_if_fail (version != NULL, NULL);

  if (conflicts != NULL)
    *conflicts = g_ptr_array_new_with_free_func (
        (GDestroyNotify) aml_modrinth_dependency_free);

  if (version->dependencies == NULL)
    return g_ptr_array_new_with_free_func (
        (GDestroyNotify) aml_modrinth_dependency_free);

  /* What @version says it does not run with, but only for what is
   * actually installed: a conflict with something that is not there is
   * nothing to decide about. */
  if (conflicts != NULL)
    {
      for (guint i = 0; i < version->dependencies->len; i++)
        {
          AmlModrinthDependency *dependency =
            g_ptr_array_index (version->dependencies, i);

          if (dependency->type != AML_DEPENDENCY_INCOMPATIBLE ||
              dependency->project_id == NULL || installed == NULL)
            continue;

          if (!g_hash_table_contains (installed, dependency->project_id))
            continue;

          g_ptr_array_add (*conflicts, aml_modrinth_dependency_copy (dependency));
        }
    }

  pending = g_ptr_array_new_with_free_func (
      (GDestroyNotify) aml_modrinth_dependency_free);

  for (guint i = 0; i < version->dependencies->len; i++)
    {
      AmlModrinthDependency *dependency =
        g_ptr_array_index (version->dependencies, i);
      gboolean already = FALSE;

      /* Embedded is inside the file, optional is a choice and
       * incompatible is a conflict: only what the version cannot run
       * without is fetched. */
      if (dependency->type != AML_DEPENDENCY_REQUIRED)
        continue;

      /* A dependency that names no project names nothing that could be
       * fetched, so the install cannot go on as if it did not exist. */
      if (dependency->project_id == NULL)
        {
          g_set_error_literal (error, AML_MODRINTH_ERROR,
                               AML_MODRINTH_ERROR_PARSE,
                               "this version requires a dependency that "
                               "names no project");
          g_ptr_array_unref (pending);
          return NULL;
        }

      if (installed != NULL &&
          g_hash_table_contains (installed, dependency->project_id))
        continue;

      /* The same project twice is one file: installing it twice would
       * leave two copies of it in the folder. */
      for (guint j = 0; j < pending->len && !already; j++)
        {
          AmlModrinthDependency *one = g_ptr_array_index (pending, j);

          already = g_strcmp0 (one->project_id, dependency->project_id) == 0;
        }

      if (already)
        continue;

      g_ptr_array_add (pending, aml_modrinth_dependency_copy (dependency));
    }

  return pending;
}
