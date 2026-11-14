/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Modrinth version, file and dependency models and parsers. */

#include "nuvashalauncher-modrinth-private.h"
#include "nuvashalauncher-modrinth-json-private.h"
#include "nuvashalauncher-json.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Versions
 * ---------------------------------------------------------- */

const char *
nuvashalauncher_modrinth_dependency_to_string (NuvashaLauncherModrinthDependencyType type)
{
  switch (type)
    {
    case nuvashalauncher_DEPENDENCY_OPTIONAL:
      return "optional";
    case nuvashalauncher_DEPENDENCY_EMBEDDED:
      return "embedded";
    case nuvashalauncher_DEPENDENCY_INCOMPATIBLE:
      return "incompatible";
    case nuvashalauncher_DEPENDENCY_REQUIRED:
    default:
      return "required";
    }
}

/* Modrinth writes nothing for "required", which is the kind an install
 * has to fetch, so a dependency without the field is that one. */
static NuvashaLauncherModrinthDependencyType
dependency_type_from_string (const char *value)
{
  if (g_strcmp0 (value, "optional") == 0)
    return nuvashalauncher_DEPENDENCY_OPTIONAL;
  if (g_strcmp0 (value, "embedded") == 0)
    return nuvashalauncher_DEPENDENCY_EMBEDDED;
  if (g_strcmp0 (value, "incompatible") == 0)
    return nuvashalauncher_DEPENDENCY_INCOMPATIBLE;

  return nuvashalauncher_DEPENDENCY_REQUIRED;
}

void
nuvashalauncher_modrinth_file_free (NuvashaLauncherModrinthFile *file)
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
nuvashalauncher_modrinth_dependency_free (NuvashaLauncherModrinthDependency *dependency)
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
NuvashaLauncherModrinthDependency *
nuvashalauncher_modrinth_dependency_copy (NuvashaLauncherModrinthDependency *dependency)
{
  NuvashaLauncherModrinthDependency *copy = NULL;

  if (dependency == NULL)
    return NULL;

  copy = g_new0 (NuvashaLauncherModrinthDependency, 1);
  copy->type = dependency->type;
  copy->project_id = g_strdup (dependency->project_id);
  copy->version_id = g_strdup (dependency->version_id);
  copy->file_name = g_strdup (dependency->file_name);

  return copy;
}

NuvashaLauncherModrinthProject *
nuvashalauncher_modrinth_project_copy (NuvashaLauncherModrinthProject *project)
{
  NuvashaLauncherModrinthProject *copy = NULL;

  if (project == NULL)
    return NULL;

  copy = g_new0 (NuvashaLauncherModrinthProject, 1);
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
nuvashalauncher_modrinth_version_free (NuvashaLauncherModrinthVersion *version)
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

NuvashaLauncherModrinthVersion *
nuvashalauncher_modrinth_version_copy (NuvashaLauncherModrinthVersion *version)
{
  NuvashaLauncherModrinthVersion *copy = NULL;

  if (version == NULL)
    return NULL;

  copy = g_new0 (NuvashaLauncherModrinthVersion, 1);
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
      (GDestroyNotify) nuvashalauncher_modrinth_file_free);
  copy->dependencies = g_ptr_array_new_with_free_func (
      (GDestroyNotify) nuvashalauncher_modrinth_dependency_free);

  for (guint i = 0; i < version->game_versions->len; i++)
    g_ptr_array_add (copy->game_versions,
                     g_strdup (g_ptr_array_index (version->game_versions, i)));

  for (guint i = 0; i < version->loaders->len; i++)
    g_ptr_array_add (copy->loaders,
                     g_strdup (g_ptr_array_index (version->loaders, i)));

  for (guint i = 0; i < version->files->len; i++)
    {
      NuvashaLauncherModrinthFile *file = g_ptr_array_index (version->files, i);
      NuvashaLauncherModrinthFile *file_copy = g_new0 (NuvashaLauncherModrinthFile, 1);

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
        nuvashalauncher_modrinth_dependency_copy (
            g_ptr_array_index (version->dependencies, i)));

  return copy;
}

NuvashaLauncherModrinthFile *
nuvashalauncher_modrinth_version_primary_file (NuvashaLauncherModrinthVersion *version)
{
  NuvashaLauncherModrinthFile *first = NULL;

  if (version == NULL || version->files == NULL)
    return NULL;

  for (guint i = 0; i < version->files->len; i++)
    {
      NuvashaLauncherModrinthFile *file = g_ptr_array_index (version->files, i);

      if (first == NULL)
        first = file;

      if (file->primary)
        return file;
    }

  return first;
}

gboolean
nuvashalauncher_modrinth_version_has_hash (const NuvashaLauncherModrinthVersion *version,
                               const char               *sha1)
{
  g_return_val_if_fail (version != NULL, FALSE);

  if (sha1 == NULL || *sha1 == '\0' || version->files == NULL)
    return FALSE;

  for (guint i = 0; i < version->files->len; i++)
    {
      const NuvashaLauncherModrinthFile *file = g_ptr_array_index (version->files, i);

      if (file->sha1 != NULL && g_ascii_strcasecmp (file->sha1, sha1) == 0)
        return TRUE;
    }

  return FALSE;
}

/* One file of a version answer, or NULL when it names no file and no
 * url, which is nothing to download. */
static NuvashaLauncherModrinthFile *
file_from_node (JsonNode *node)
{
  g_autofree char *filename = nuvashalauncher_json_string (node, "filename");
  g_autofree char *url = nuvashalauncher_json_string (node, "url");
  g_autofree char *sha1 = NULL;
  g_autofree char *sha512 = NULL;
  NuvashaLauncherModrinthFile *file = NULL;

  if (filename == NULL || url == NULL)
    return NULL;

  {
    JsonNode *hashes = json_object_get_member (nuvashalauncher_json_object_of (node), "hashes");

    if (hashes != NULL)
      {
        sha1 = nuvashalauncher_json_string (hashes, "sha1");
        sha512 = nuvashalauncher_json_string (hashes, "sha512");
      }
  }

  file = g_new0 (NuvashaLauncherModrinthFile, 1);
  file->filename = g_steal_pointer (&filename);
  file->url = g_steal_pointer (&url);
  file->sha1 = g_steal_pointer (&sha1);
  file->sha512 = g_steal_pointer (&sha512);
  file->primary = nuvashalauncher_json_boolean (node, "primary", FALSE);
  file->size = (goffset) nuvashalauncher_json_int (node, "size", 0);

  return file;
}

static NuvashaLauncherModrinthDependency *
dependency_from_node (JsonNode *node)
{
  g_autofree char *type = nuvashalauncher_json_string (node, "dependency_type");
  NuvashaLauncherModrinthDependency *dependency = g_new0 (NuvashaLauncherModrinthDependency, 1);

  dependency->type = dependency_type_from_string (type);
  dependency->project_id = nuvashalauncher_json_string (node, "project_id");
  dependency->version_id = nuvashalauncher_json_string (node, "version_id");
  dependency->file_name = nuvashalauncher_json_string (node, "file_name");

  return dependency;
}

/* One version answer, or NULL when there is nothing installable in it:
 * without an id it cannot be asked about again, and without a file
 * there is nothing to download. */
static NuvashaLauncherModrinthVersion *
version_from_node (JsonNode *node)
{
  NuvashaLauncherModrinthVersion *version = NULL;
  JsonNode *array_node = NULL;

  if (nuvashalauncher_json_object_of (node) == NULL)
    return NULL;

  version = g_new0 (NuvashaLauncherModrinthVersion, 1);
  version->version_id = nuvashalauncher_json_string (node, "id");
  version->project_id = nuvashalauncher_json_string (node, "project_id");
  version->name = nuvashalauncher_json_string (node, "name");
  version->version_number = nuvashalauncher_json_string (node, "version_number");
  version->changelog = nuvashalauncher_json_string (node, "changelog");
  version->date_published = nuvashalauncher_json_string (node, "date_published");
  version->downloads = (guint64) nuvashalauncher_json_int (node, "downloads", 0);
  version->game_versions = nuvashalauncher_json_string_array (node, "game_versions");
  version->loaders = nuvashalauncher_json_string_array (node, "loaders");

  version->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_modrinth_file_free);
  array_node = json_object_get_member (nuvashalauncher_json_object_of (node), "files");

  if (array_node != NULL && JSON_NODE_HOLDS_ARRAY (array_node))
    {
      JsonArray *array = json_node_get_array (array_node);
      guint length = json_array_get_length (array);

      for (guint i = 0; i < length; i++)
        {
          NuvashaLauncherModrinthFile *file =
            file_from_node (json_array_get_element (array, i));

          if (file != NULL)
            g_ptr_array_add (version->files, file);
        }
    }

  version->dependencies = g_ptr_array_new_with_free_func (
      (GDestroyNotify) nuvashalauncher_modrinth_dependency_free);
  array_node = json_object_get_member (nuvashalauncher_json_object_of (node), "dependencies");

  if (array_node != NULL && JSON_NODE_HOLDS_ARRAY (array_node))
    {
      JsonArray *array = json_node_get_array (array_node);
      guint length = json_array_get_length (array);

      for (guint i = 0; i < length; i++)
        {
          JsonNode *item = json_array_get_element (array, i);

          if (nuvashalauncher_json_object_of (item) != NULL)
            g_ptr_array_add (version->dependencies, dependency_from_node (item));
        }
    }

  if (version->version_id == NULL || version->files->len == 0)
    {
      nuvashalauncher_modrinth_version_free (version);
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

NuvashaLauncherModrinthVersion *
nuvashalauncher_modrinth_version_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonNode) root = nuvashalauncher_modrinth_root_node (json, length, error);
  NuvashaLauncherModrinthVersion *version = NULL;

  if (root == NULL)
    return NULL;

  version = version_from_node (root);

  if (version == NULL)
    {
      g_set_error_literal (error, nuvashalauncher_MODRINTH_ERROR,
                           nuvashalauncher_MODRINTH_ERROR_PARSE,
                           "the answer is not a version with a file");
      return NULL;
    }

  return version;
}

GHashTable *
nuvashalauncher_modrinth_updates_from_node (JsonNode *root, GError **error)
{
  JsonObject *object = nuvashalauncher_json_object_of (root);
  g_autoptr (GHashTable) updates = NULL;
  g_autoptr (GList) keys = NULL;

  if (object == NULL)
    {
      g_set_error_literal (error, nuvashalauncher_MODRINTH_ERROR,
                           nuvashalauncher_MODRINTH_ERROR_PARSE,
                           "the updates answer is not a JSON object");
      return NULL;
    }

  updates = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify) nuvashalauncher_modrinth_version_free);
  keys = json_object_get_members (object);

  for (GList *l = keys; l != NULL; l = l->next)
    {
      const char *hash = l->data;
      JsonNode *member = json_object_get_member (object, hash);
      NuvashaLauncherModrinthVersion *version = NULL;

      if (hash == NULL || *hash == '\0' || member == NULL ||
          JSON_NODE_HOLDS_NULL (member))
        continue;

      version = version_from_node (member);
      if (version != NULL)
        g_hash_table_insert (updates, g_strdup (hash), version);
    }

  return g_steal_pointer (&updates);
}

GHashTable *
nuvashalauncher_modrinth_updates_from_json (const char *json, gssize length,
                                            GError **error)
{
  g_autoptr (JsonNode) root = nuvashalauncher_modrinth_root_node (json, length, error);

  if (root == NULL)
    return NULL;

  return nuvashalauncher_modrinth_updates_from_node (root, error);
}

/* The one rule of compatibility.
 *
 * A mod runs on one Minecraft version and one loader; a resource pack
 * and a shader pack are loaded by the game itself, so the Minecraft
 * version is all that decides them. FALSE is the answer that keeps an
 * incompatible version out of every list. */
gboolean
nuvashalauncher_modrinth_version_fits (NuvashaLauncherModrinthVersion *version, NuvashaLauncherContentType type,
                          const char *minecraft_version, const char *loader)
{
  if (version == NULL || minecraft_version == NULL ||
      *minecraft_version == '\0')
    return FALSE;

  if (!nuvashalauncher_modrinth_string_array_has (version->game_versions, minecraft_version))
    return FALSE;

  if (!nuvashalauncher_content_type_needs_loader (type) || loader == NULL ||
      *loader == '\0')
    return TRUE;

  /* A mod that names no loader is one the loader runs itself, so it is
   * nothing to offer for an instance that has one. */
  if (version->loaders == NULL || version->loaders->len == 0)
    return FALSE;

  for (guint i = 0; i < version->loaders->len; i++)
    {
      if (nuvashalauncher_modrinth_loader_runs (loader, g_ptr_array_index (version->loaders, i)))
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
  const NuvashaLauncherModrinthVersion *left = *(const NuvashaLauncherModrinthVersion *const *) a;
  const NuvashaLauncherModrinthVersion *right = *(const NuvashaLauncherModrinthVersion *const *) b;

  return strcmp (right->date_published, left->date_published);
}

GPtrArray *
nuvashalauncher_modrinth_versions_from_json (const char *json, gssize length,
                                 NuvashaLauncherContentType type,
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
      g_set_error_literal (error, nuvashalauncher_MODRINTH_ERROR,
                           nuvashalauncher_MODRINTH_ERROR_PARSE,
                           "the versions answer is not a list");
      return NULL;
    }

  versions = g_ptr_array_new_with_free_func (
      (GDestroyNotify) nuvashalauncher_modrinth_version_free);
  array = json_node_get_array (root);
  count = json_array_get_length (array);

  for (guint i = 0; i < count; i++)
    {
      NuvashaLauncherModrinthVersion *version =
        version_from_node (json_array_get_element (array, i));

      if (version == NULL)
        continue;

      /* The server was already asked for these, so what is left here is
       * the check that keeps an incompatible version out of the list. */
      if (!nuvashalauncher_modrinth_version_fits (version, type, minecraft_version,
                                      loader))
        {
          nuvashalauncher_modrinth_version_free (version);
          continue;
        }

      g_ptr_array_add (versions, version);
      compatible++;
    }

  if (compatible == 0)
    {
      g_autofree char *message = nuvashalauncher_modrinth_no_compatible_message (
          type, minecraft_version, loader);

      g_ptr_array_unref (versions);
      g_set_error (error, nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_NOT_FOUND,
                   "%s", message);
      return NULL;
    }

  g_ptr_array_sort (versions, compare_by_date);

  return versions;
}
