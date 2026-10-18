/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Modrinth data objects, compatibility rules and JSON parsers. Network
 * orchestration lives in nuvasha-modrinth.c. */

#include "nuvasha-modrinth.h"
#include "nuvasha-modrinth-private.h"
#include "nuvasha-modrinth-json-private.h"

#include "nuvasha-i18n.h"
#include "nuvasha-json.h"

#include <json-glib/json-glib.h>
#include <string.h>

GQuark
nuvasha_modrinth_error_quark (void)
{
  return g_quark_from_static_string ("nuvasha-modrinth-error");
}

char *
nuvasha_modrinth_json_node_to_string (JsonNode *node)
{
  g_return_val_if_fail (node != NULL, NULL);
  return json_to_string (node, FALSE);
}

/* ---------------------------------------------------------- *
 * The kinds of content
 * ---------------------------------------------------------- */

const char *
nuvasha_content_type_to_string (NuvashaContentType type)
{
  switch (type)
    {
    case nuvasha_CONTENT_RESOURCE_PACK:
      return "resourcepack";
    case nuvasha_CONTENT_SHADER_PACK:
      return "shader";
    case nuvasha_CONTENT_MOD:
    default:
      return "mod";
    }
}

const char *
nuvasha_content_type_title (NuvashaContentType type)
{
  switch (type)
    {
    case nuvasha_CONTENT_RESOURCE_PACK:
      return _("Resource Packs");
    case nuvasha_CONTENT_SHADER_PACK:
      return _("Shader Packs");
    case nuvasha_CONTENT_MOD:
    default:
      return _("Mods");
    }
}

const char *
nuvasha_content_type_folder (NuvashaContentType type)
{
  switch (type)
    {
    case nuvasha_CONTENT_RESOURCE_PACK:
      return "resourcepacks";
    case nuvasha_CONTENT_SHADER_PACK:
      return "shaderpacks";
    case nuvasha_CONTENT_MOD:
    default:
      return "mods";
    }
}

gboolean
nuvasha_content_type_needs_loader (NuvashaContentType type)
{
  return type == nuvasha_CONTENT_MOD;
}

const char *
nuvasha_content_type_extension (NuvashaContentType type)
{
  return nuvasha_content_type_needs_loader (type) ? ".jar" : ".zip";
}

const char *
nuvasha_content_type_icon (NuvashaContentType type)
{
  switch (type)
    {
    case nuvasha_CONTENT_RESOURCE_PACK:
      return "image-x-generic-symbolic";
    case nuvasha_CONTENT_SHADER_PACK:
      return "applications-graphics-symbolic";
    case nuvasha_CONTENT_MOD:
    default:
      return "view-list-symbolic";
    }
}

/* ---------------------------------------------------------- *
 * Reading a JSON answer
 * ---------------------------------------------------------- */

JsonNode *
nuvasha_modrinth_root_node (const char *json, gssize length, GError **error)
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
      g_set_error_literal (error, nuvasha_MODRINTH_ERROR,
                           nuvasha_MODRINTH_ERROR_PARSE,
                           "the answer holds no JSON");
      return NULL;
    }

  /* The copy is what outlives the parser. */
  return json_node_copy (root);
}

/* The string @name of the object @node, or NULL. A field that is there
 * but is not a string counts as absent, so a malformed answer cannot
 * reach a list as an empty string where words were meant. */
gboolean
nuvasha_modrinth_string_array_has (GPtrArray *strings, const char *value)
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
char *
nuvasha_modrinth_string_array_json (const char *value)
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
gboolean
nuvasha_modrinth_loader_runs (const char *instance_loader, const char *version_loader)
{
  if (g_strcmp0 (instance_loader, version_loader) == 0)
    return TRUE;

  return g_strcmp0 (instance_loader, "quilt") == 0 &&
         g_strcmp0 (version_loader, "fabric") == 0;
}

/* The loaders a request for @loader asks Modrinth for, as the JSON array
 * it takes: a Quilt instance asks for Fabric versions as well. */
char *
nuvasha_modrinth_loaders_json (const char *loader)
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
nuvasha_modrinth_project_free (NuvashaModrinthProject *project)
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
static NuvashaModrinthProject *
project_from_hit (JsonNode *hit, GError **error)
{
  NuvashaModrinthProject *project = g_new0 (NuvashaModrinthProject, 1);

  if (!nuvasha_json_require_string (hit, "project_id", &project->project_id,
                                error) ||
      !nuvasha_json_require_string (hit, "slug", &project->slug, error))
    {
      nuvasha_modrinth_project_free (project);
      return NULL;
    }

  project->title = nuvasha_json_string (hit, "title");
  project->description = nuvasha_json_string (hit, "description");
  project->author = nuvasha_json_string (hit, "author");
  project->icon_url = nuvasha_json_string (hit, "icon_url");
  project->downloads = (guint64) nuvasha_json_int (hit, "downloads", 0);
  project->followers = (guint64) nuvasha_json_int (hit, "follows", 0);
  project->versions = nuvasha_json_string_array (hit, "versions");
  project->categories = nuvasha_json_string_array (hit, "categories");

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
nuvasha_modrinth_details_free (NuvashaModrinthDetails *details)
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

NuvashaModrinthDetails *
nuvasha_modrinth_details_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonNode) root = nuvasha_modrinth_root_node (json, length, error);
  JsonNode *team = NULL; /* borrowed from @root */
  NuvashaModrinthDetails *details = NULL;

  if (root == NULL)
    return NULL;

  details = g_new0 (NuvashaModrinthDetails, 1);
  details->project_id = nuvasha_json_string (root, "id");
  details->slug = nuvasha_json_string (root, "slug");
  details->title = nuvasha_json_string (root, "title");
  details->description = nuvasha_json_string (root, "description");
  details->body = nuvasha_json_string (root, "body");
  details->icon_url = nuvasha_json_string (root, "icon_url");
  details->source_url = nuvasha_json_string (root, "source_url");
  details->issues_url = nuvasha_json_string (root, "issues_url");
  details->wiki_url = nuvasha_json_string (root, "wiki_url");
  details->downloads = (guint64) nuvasha_json_int (root, "downloads", 0);
  details->followers = (guint64) nuvasha_json_int (root, "followers", 0);
  details->game_versions = nuvasha_json_string_array (root, "game_versions");
  details->categories = nuvasha_json_string_array (root, "categories");
  details->loaders = nuvasha_json_string_array (root, "loaders");

  /* The license is an object with an id, not a string. */
  details->license =
    nuvasha_json_string (json_object_get_member (nuvasha_json_object_of (root), "license"), "id");

  /* A search answer names an author; a project answer carries the team
   * instead, and Modrinth writes that as the team's project id rather
   * than as its members, so there is no name to take from it. Members
   * are read anyway in case that ever stops being true. */
  details->author = nuvasha_json_string (root, "author");

  if (details->author == NULL)
    {
      team = json_object_get_member (nuvasha_json_object_of (root), "team");

      if (team != NULL && JSON_NODE_HOLDS_ARRAY (team) &&
          json_array_get_length (json_node_get_array (team)) > 0)
        details->author = nuvasha_json_string (
            json_array_get_element (json_node_get_array (team), 0), "name");
    }

  if (details->project_id == NULL)
    {
      g_set_error_literal (error, nuvasha_MODRINTH_ERROR,
                           nuvasha_MODRINTH_ERROR_PARSE,
                           "the project answer holds no id");
      nuvasha_modrinth_details_free (details);
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
nuvasha_modrinth_search_free (NuvashaModrinthSearch *search)
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
nuvasha_modrinth_results_free (NuvashaModrinthResults *results)
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
nuvasha_modrinth_facets (const NuvashaModrinthSearch *search)
{
  g_autoptr (GPtrArray) facets =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_val_if_fail (search != NULL, g_strdup ("[[\"project_type:mod\"]]"));

  facet_add (facets, "project_type", nuvasha_content_type_to_string (search->type));

  if (search->minecraft_version != NULL && *search->minecraft_version != '\0')
    facet_add (facets, "versions", search->minecraft_version);

  /* Only a mod is narrowed by loader: the game loads a resource pack
   * and a shader pack itself, whatever runs it. */
  if (nuvasha_content_type_needs_loader (search->type) && search->loader != NULL &&
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
nuvasha_modrinth_search_url (const NuvashaModrinthSearch *search)
{
  g_autofree char *facets = NULL;
  g_autofree char *query = NULL;
  g_autofree char *escaped = NULL;

  g_return_val_if_fail (search != NULL, NULL);

  facets = nuvasha_modrinth_facets (search);
  query = g_uri_escape_string (search->query != NULL ? search->query : "",
                              NULL, FALSE);
  escaped = g_uri_escape_string (facets, NULL, FALSE);

  return g_strdup_printf (
      nuvasha_MODRINTH_URL "/search?query=%s&facets=%s&offset=%u&limit=%u", query,
      escaped, search->offset, search->limit > 0 ? search->limit : 20);
}

NuvashaModrinthResults *
nuvasha_modrinth_results_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonNode) root = nuvasha_modrinth_root_node (json, length, error);
  NuvashaModrinthResults *results = NULL;
  JsonNode *hits = NULL;

  if (root == NULL)
    return NULL;

  hits = json_object_get_member (nuvasha_json_object_of (root), "hits");

  if (hits == NULL || !JSON_NODE_HOLDS_ARRAY (hits))
    {
      g_set_error_literal (error, nuvasha_MODRINTH_ERROR,
                           nuvasha_MODRINTH_ERROR_PARSE,
                           "the search answer holds no hits");
      return NULL;
    }

  results = g_new0 (NuvashaModrinthResults, 1);
  results->projects =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvasha_modrinth_project_free);
  results->total_hits = (guint64) nuvasha_json_int (root, "total_hits", 0);

  {
    JsonArray *array = json_node_get_array (hits);
    guint hit_count = json_array_get_length (array);

    for (guint i = 0; i < hit_count; i++)
      {
        JsonNode *hit = json_array_get_element (array, i);

        /* A hit that is not an object has no title and no id, so there
         * is nothing a row could show for it. */
        if (nuvasha_json_object_of (hit) == NULL)
          continue;

        {
          g_autoptr (GError) result_error = NULL;
          NuvashaModrinthProject *project = project_from_hit (hit, &result_error);

          if (project == NULL)
            {
              g_set_error (error, nuvasha_MODRINTH_ERROR, nuvasha_MODRINTH_ERROR_PARSE,
                           "invalid search result: %s",
                           result_error != NULL ? result_error->message
                                                 : "invalid project");
              nuvasha_modrinth_results_free (results);
              return NULL;
            }

          g_ptr_array_add (results->projects, project);
         }
      }
  }

  return results;
}
