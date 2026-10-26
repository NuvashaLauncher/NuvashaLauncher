/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Modrinth API v2 support, using asynchronous requests from the download
 * module.
 *
 * The endpoints this uses, all of them GET unless said otherwise:
 *
 *   /search                       Browse results
 *   /project/<id>                 project details
 *   /project/<id>/version         project versions,
 *                                 already narrowed by game_versions
 *                                 and loaders
 *   /version_file/<sha1>          which version a file on disk is
 *   /version_file/<sha1>/update   POST, whether that file has something
 *                                 newer that fits this instance
 *
 * Server-side filtering is checked again here. The shared
 * nuvashalauncher_modrinth_version_fits() rule prevents incompatible versions from
 * reaching any list.
 *
 * Callbacks run in the caller's thread-default main context. Parsing happens
 * after the download completes; this module does not block the main loop. */

#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-modrinth-private.h"
#include "nuvashalauncher-task-private.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-json.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Searches
 * ---------------------------------------------------------- */

static void
search_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  nuvashalauncherModrinthResults *results = NULL;

  results = nuvashalauncher_modrinth_results_from_json (json, -1, &error);

  if (results == NULL)
    {
      nuvashalauncher_modrinth_request_return_error (task, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_modrinth_request_return_pointer (task, results,
                         (GDestroyNotify) nuvashalauncher_modrinth_results_free);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_search_async (const nuvashalauncherModrinthSearch *search,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (search != NULL, NULL);

  url = nuvashalauncher_modrinth_search_url (search);
  return nuvashalauncher_modrinth_get_async (url, callback, user_data, search_answer,
                    nuvashalauncher_NOT_FOUND_IS_ERROR);
}

nuvashalauncherModrinthResults *
nuvashalauncher_modrinth_search_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
details_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  nuvashalauncherModrinthDetails *details = NULL;

  details = nuvashalauncher_modrinth_details_from_json (json, -1, &error);

  if (details == NULL)
    {
      nuvashalauncher_modrinth_request_return_error (task, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_modrinth_request_return_pointer (task, details,
                         (GDestroyNotify) nuvashalauncher_modrinth_details_free);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_details_async (const char *project_id, GAsyncReadyCallback callback,
                            gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);

  {
    g_autofree char *escaped_id =
      g_uri_escape_string (project_id, NULL, FALSE);

    url = g_strdup_printf (nuvashalauncher_MODRINTH_URL "/project/%s", escaped_id);
  }
  return nuvashalauncher_modrinth_get_async (url, callback, user_data, details_answer,
                    nuvashalauncher_NOT_FOUND_IS_ERROR);
}
nuvashalauncherModrinthDetails *
nuvashalauncher_modrinth_details_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The versions of a project
 * ---------------------------------------------------------- */

/* Instance context used to validate a request's response. */
typedef struct {
  nuvashalauncherContentType type;
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
instance_ask_new (nuvashalauncherContentType type, const char *minecraft_version,
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
  nuvashalauncherModrinthRequest *request = g_task_get_task_data (task);
  InstanceAsk *asked = request->data;
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  GPtrArray *versions = NULL;

  versions = nuvashalauncher_modrinth_versions_from_json (json, -1, asked->type,
                                              asked->minecraft_version,
                                              asked->loader, &error);

  if (versions == NULL)
    {
      nuvashalauncher_modrinth_request_return_error (task, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_modrinth_request_return_pointer (task, versions, (GDestroyNotify) g_ptr_array_unref);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_versions_async (const char *project_id, nuvashalauncherContentType type,
                             const char *minecraft_version, const char *loader,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *escaped_id = NULL;
  g_autofree char *escaped_game = NULL;
  g_autofree char *escaped_loaders = NULL;
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  game_versions = nuvashalauncher_modrinth_string_array_json (minecraft_version);
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
  if (nuvashalauncher_content_type_needs_loader (type) && loader != NULL &&
      *loader != '\0')
    {
      loaders = nuvashalauncher_modrinth_loaders_json (loader);
      escaped_loaders = g_uri_escape_string (loaders, NULL, FALSE);
    }

  if (escaped_loaders != NULL)
    url = g_strdup_printf (
        nuvashalauncher_MODRINTH_URL "/project/%s/version?game_versions=%s&loaders=%s",
        escaped_id, escaped_game, escaped_loaders);
  else
    url = g_strdup_printf (
        nuvashalauncher_MODRINTH_URL "/project/%s/version?game_versions=%s",
        escaped_id, escaped_game);

  scope = nuvashalauncher_modrinth_request_new (callback, user_data, versions_answer,
                       nuvashalauncher_NOT_FOUND_IS_ERROR,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_json_cached_async (nuvashalauncher_downloads_get_service (), url, nuvashalauncher_modrinth_json_ready,
                                    nuvashalauncher_task_scope_task (scope)),
    nuvashalauncher_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvashalauncher_modrinth_versions_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
categories_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  nuvashalauncherModrinthRequest *request = g_task_get_task_data (task);
  GPtrArray *categories = NULL;

  categories = nuvashalauncher_modrinth_categories_from_json (
      json, -1, (const char *) request->data, &error);

  if (categories == NULL)
    {
      nuvashalauncher_modrinth_request_return_error (task, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_modrinth_request_return_pointer (task, categories, (GDestroyNotify) g_ptr_array_unref);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_categories_async (const char        *project_type,
                               GAsyncReadyCallback callback,
                               gpointer           user_data)
{
  g_autofree char *url = g_strdup_printf (nuvashalauncher_MODRINTH_URL "/tag/category");
  g_autoptr (nuvashalauncherTaskScope) scope = NULL;

  scope = nuvashalauncher_modrinth_request_new (callback, user_data, categories_answer,
                       nuvashalauncher_NOT_FOUND_IS_ERROR, g_strdup (project_type), g_free);
  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_json_cached_async (nuvashalauncher_downloads_get_service (), url, nuvashalauncher_modrinth_json_ready,
                                    nuvashalauncher_task_scope_task (scope)),
    nuvashalauncher_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvashalauncher_modrinth_categories_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

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
  nuvashalauncherModrinthVersion *version = NULL;

  version = nuvashalauncher_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      nuvashalauncher_modrinth_request_return_error (task, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_modrinth_request_return_pointer (task, version,
                         (GDestroyNotify) nuvashalauncher_modrinth_version_free);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_version_by_hash_async (const char *sha1,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (nuvashalauncher_MODRINTH_URL "/version_file/%s?algorithm=sha1",
                           escaped_sha1);
  }
  return nuvashalauncher_modrinth_get_async (url, callback, user_data, version_answer,
                    nuvashalauncher_NOT_FOUND_IS_ERROR);
}

nuvashalauncherModrinthVersion *
nuvashalauncher_modrinth_version_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_version_by_id_async (const char *version_id,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  g_autofree char *escaped = g_uri_escape_string (version_id, NULL, FALSE);
  g_autofree char *url = g_strdup_printf (nuvashalauncher_MODRINTH_URL "/version/%s",
                                          escaped);

  return nuvashalauncher_modrinth_get_async (url, callback, user_data, version_answer,
                    nuvashalauncher_NOT_FOUND_IS_ERROR);
}

/* ---------------------------------------------------------- *
 * An update out of the hash of a file
 * ---------------------------------------------------------- */

static void
update_answer (GTask *task, JsonNode *root)
{
  nuvashalauncherModrinthRequest *request = g_task_get_task_data (task);
  InstanceAsk *asked = request->data;
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  nuvashalauncherModrinthVersion *version = NULL;

  version = nuvashalauncher_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      nuvashalauncher_modrinth_request_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* Modrinth was asked for something that fits this instance, so
   * anything else it answered is not an update this instance can take. */
  if (!nuvashalauncher_modrinth_version_fits (version, asked->type,
                                  asked->minecraft_version, asked->loader))
    {
      nuvashalauncher_modrinth_version_free (version);
      nuvashalauncher_modrinth_request_return_pointer (task, NULL, NULL);
      return;
    }

  nuvashalauncher_modrinth_request_return_pointer (task, version,
                         (GDestroyNotify) nuvashalauncher_modrinth_version_free);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_update_by_hash_async (nuvashalauncherContentType type, const char *sha1,
                                   const char *minecraft_version,
                                   const char *loader,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope = NULL;
  g_autofree char *url = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *body = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  /* Both narrowings are already JSON arrays, so they can be inserted directly
   * into the request body. */
  game_versions = nuvashalauncher_modrinth_string_array_json (minecraft_version);
  loaders = nuvashalauncher_modrinth_string_array_json (
    nuvashalauncher_content_type_needs_loader (type) && loader != NULL ? loader
                                                            : "minecraft");

  body = g_strdup_printf ("{\"game_versions\":%s,\"loaders\":%s}",
                          game_versions, loaders);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (
        nuvashalauncher_MODRINTH_URL "/version_file/%s/update?algorithm=sha1",
        escaped_sha1);
  }

  scope = nuvashalauncher_modrinth_request_new (callback, user_data, update_answer,
                       nuvashalauncher_NOT_FOUND_IS_ABSENT,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_json_post_cached_async (nuvashalauncher_downloads_get_service (), url, body, nuvashalauncher_MODRINTH_USER_AGENT,
                                         nuvashalauncher_modrinth_json_ready,
                                         nuvashalauncher_task_scope_task (scope)),
    nuvashalauncher_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

nuvashalauncherModrinthVersion *
nuvashalauncher_modrinth_update_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Dependencies required to install a version
 * ---------------------------------------------------------- */

char *
nuvashalauncher_modrinth_no_compatible_message (nuvashalauncherContentType type,
                                    const char *minecraft_version,
                                    const char *loader)
{
  const char *version = minecraft_version != NULL ? minecraft_version : "";

  if (nuvashalauncher_content_type_needs_loader (type))
    return g_strdup_printf (_("No compatible version found for Minecraft %s and %s."),
                            version, loader != NULL ? loader : "");

  return g_strdup_printf (_("No compatible version found for Minecraft %s."),
                          version);
}

GPtrArray *
nuvashalauncher_modrinth_resolve_install (nuvashalauncherModrinthVersion *version,
                              GHashTable *installed, GPtrArray **conflicts,
                              GError **error)
{
  GPtrArray *pending = NULL;

  g_return_val_if_fail (version != NULL, NULL);

  if (conflicts != NULL)
    *conflicts = g_ptr_array_new_with_free_func (
        (GDestroyNotify) nuvashalauncher_modrinth_dependency_free);

  if (version->dependencies == NULL)
    return g_ptr_array_new_with_free_func (
        (GDestroyNotify) nuvashalauncher_modrinth_dependency_free);

  /* Check conflicts only against content installed in this instance. */
  if (conflicts != NULL)
    {
      for (guint i = 0; i < version->dependencies->len; i++)
        {
          nuvashalauncherModrinthDependency *dependency =
            g_ptr_array_index (version->dependencies, i);

          if (dependency->type != nuvashalauncher_DEPENDENCY_INCOMPATIBLE ||
              dependency->project_id == NULL || installed == NULL)
            continue;

          if (!g_hash_table_contains (installed, dependency->project_id))
            continue;

          g_ptr_array_add (*conflicts, nuvashalauncher_modrinth_dependency_copy (dependency));
        }
    }

  pending = g_ptr_array_new_with_free_func (
      (GDestroyNotify) nuvashalauncher_modrinth_dependency_free);

  for (guint i = 0; i < version->dependencies->len; i++)
    {
      nuvashalauncherModrinthDependency *dependency =
        g_ptr_array_index (version->dependencies, i);
      gboolean already = FALSE;

      /* Embedded is inside the file, optional is a choice and
       * incompatible is a conflict: only dependencies the version cannot run
       * without is fetched. */
      if (dependency->type != nuvashalauncher_DEPENDENCY_REQUIRED)
        continue;

      /* A dependency that names no project names nothing that could be
       * fetched, so the install cannot go on as if it did not exist. */
      if (dependency->project_id == NULL)
        {
          g_set_error_literal (error, nuvashalauncher_MODRINTH_ERROR,
                               nuvashalauncher_MODRINTH_ERROR_PARSE,
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
          nuvashalauncherModrinthDependency *one = g_ptr_array_index (pending, j);

          already = g_strcmp0 (one->project_id, dependency->project_id) == 0;
        }

      if (already)
        continue;

      g_ptr_array_add (pending, nuvashalauncher_modrinth_dependency_copy (dependency));
    }

  return pending;
}
