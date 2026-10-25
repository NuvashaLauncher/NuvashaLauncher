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
 * nuvasha_modrinth_version_fits() rule prevents incompatible versions from
 * reaching any list.
 *
 * Callbacks run in the caller's thread-default main context. Parsing happens
 * after the download completes; this module does not block the main loop. */

#include "nuvasha-modrinth.h"
#include "nuvasha-modrinth-private.h"
#include "nuvasha-task-private.h"

#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-i18n.h"
#include "nuvasha-json.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * The requests
 * ---------------------------------------------------------- */

/* Callback receiving a parsed response. */
typedef void (*AnswerFunc) (GTask *task, JsonNode *root);

/* Whether HTTP 404 is an error or an absent resource for a request. */
typedef enum {
  nuvasha_NOT_FOUND_IS_ERROR, /* the answer came from a known place */
  nuvasha_NOT_FOUND_IS_ABSENT
} NuvashaNotFoundRule;

typedef struct {
  AnswerFunc answer;
  NuvashaNotFoundRule not_found;
  NuvashaTaskScope *scope;
  void *data;
  GDestroyNotify free_data;
} Request;

typedef struct {
  char *key;
  GPtrArray *tasks;
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
  nuvasha_task_scope_return_error (request->scope, error);
}

static void
request_return_pointer (GTask *task, gpointer value, GDestroyNotify free_value)
{
  Request *request = g_task_get_task_data (task);
  nuvasha_task_scope_return_pointer (request->scope, value, free_value);
}

static NuvashaTaskScope *
request_new (GAsyncReadyCallback callback, gpointer user_data,
             AnswerFunc answer, NuvashaNotFoundRule not_found, void *data,
             GDestroyNotify free_data)
{
  NuvashaTaskScope *scope = nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvasha_task_scope_task (scope);
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

/* Return an error in this module's domain while preserving the failed URL
 * and download error details. */
static void
fail (GTask *task, GError *error)
{
  Request *request = g_task_get_task_data (task);
  GError *wrapped = NULL;

  if (g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
      g_clear_error (&error);

      nuvasha_task_scope_return_error (
        request->scope,
        g_error_new_literal (nuvasha_MODRINTH_ERROR,
                             nuvasha_MODRINTH_ERROR_NETWORK,
                             "the request was cancelled"));
      return;
    }

  if (error->domain == nuvasha_DOWNLOAD_ERROR)
    {
      if (error->code == nuvasha_DOWNLOAD_ERROR_RATE_LIMITED)
        wrapped = g_error_new_literal (
            nuvasha_MODRINTH_ERROR, nuvasha_MODRINTH_ERROR_RATE_LIMIT,
            _("Modrinth is asking Nuvasha to slow down, too many requests"));
      else if (error->code == nuvasha_DOWNLOAD_ERROR_HTTP)
        wrapped = g_error_new (nuvasha_MODRINTH_ERROR, nuvasha_MODRINTH_ERROR_HTTP,
                               "%s", error->message);
      else
        wrapped = g_error_new (nuvasha_MODRINTH_ERROR, nuvasha_MODRINTH_ERROR_NETWORK,
                               "%s", error->message);
    }
  else
    wrapped = g_error_new (nuvasha_MODRINTH_ERROR, nuvasha_MODRINTH_ERROR_PARSE,
                           "%s", error->message);

  g_error_free (error);
  nuvasha_task_scope_return_error (request->scope, wrapped);
}

/* Check whether a download error represents HTTP 404. */
static gboolean
error_is_not_found (const GError *error)
{
  return error->domain == nuvasha_DOWNLOAD_ERROR &&
         error->code == nuvasha_DOWNLOAD_ERROR_HTTP &&
         strstr (error->message, "HTTP 404") != NULL;
}

static void
json_flight_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  JsonFlight *flight = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = nuvasha_download_json_finish (result, &error);
  JsonNode *root = parser != NULL ? json_parser_get_root (parser) : NULL;

  /* Remove before delivering callbacks: a callback may immediately request
   * the same URL again, and that request must start a fresh flight. */
  g_hash_table_steal (json_flights, flight->key);

  for (guint i = 0; i < flight->tasks->len; i++)
    {
      GTask *task = g_ptr_array_index (flight->tasks, i);
      Request *request = g_task_get_task_data (task);

      if (nuvasha_task_scope_return_error_if_cancelled (request->scope))
        continue;

      if (parser == NULL)
        {
          if (error_is_not_found (error) &&
              request->not_found == nuvasha_NOT_FOUND_IS_ABSENT)
            {
               nuvasha_task_scope_return_pointer (request->scope, NULL, NULL);
              continue;
            }

          if (error_is_not_found (error))
            {
               nuvasha_task_scope_return_error (
                 request->scope,
                 g_error_new_literal (nuvasha_MODRINTH_ERROR,
                                      nuvasha_MODRINTH_ERROR_NOT_FOUND,
                                       _("Modrinth does not know this file")));
              continue;
            }

          fail (task, g_error_copy (error));
          continue;
        }

      if (root == NULL)
        {
           nuvasha_task_scope_return_error (
             request->scope,
             g_error_new_literal (nuvasha_MODRINTH_ERROR,
                                  nuvasha_MODRINTH_ERROR_PARSE,
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

  parser = nuvasha_download_json_finish (result, &error);

  if (parser == NULL)
    {
      if (error_is_not_found (error) &&
          request->not_found == nuvasha_NOT_FOUND_IS_ABSENT)
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
            g_error_new_literal (nuvasha_MODRINTH_ERROR,
                                 nuvasha_MODRINTH_ERROR_NOT_FOUND,
                                  _("Modrinth does not know this file")));
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
        g_error_new_literal (nuvasha_MODRINTH_ERROR, nuvasha_MODRINTH_ERROR_PARSE,
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

static NuvashaTaskScope *
get_async (const char *url, GAsyncReadyCallback callback, gpointer user_data,
           AnswerFunc answer, NuvashaNotFoundRule not_found)
{
  g_autoptr (NuvashaTaskScope) scope =
    request_new (callback, user_data, answer, not_found, NULL, NULL);
  GTask *task = nuvasha_task_scope_task (scope);
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

  nuvasha_task_scope_release_when_done (
    nuvasha_download_json_cached_async (nuvasha_downloads_get_service (), url, json_flight_done, flight),
    nuvasha_task_scope_get_cancellable (scope));

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
  NuvashaModrinthResults *results = NULL;

  results = nuvasha_modrinth_results_from_json (json, -1, &error);

  if (results == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, results,
                         (GDestroyNotify) nuvasha_modrinth_results_free);
}

NuvashaTaskScope *
nuvasha_modrinth_search_async (const NuvashaModrinthSearch *search,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (search != NULL, NULL);

  url = nuvasha_modrinth_search_url (search);
  return get_async (url, callback, user_data, search_answer,
                    nuvasha_NOT_FOUND_IS_ERROR);
}

NuvashaModrinthResults *
nuvasha_modrinth_search_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
details_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  NuvashaModrinthDetails *details = NULL;

  details = nuvasha_modrinth_details_from_json (json, -1, &error);

  if (details == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, details,
                         (GDestroyNotify) nuvasha_modrinth_details_free);
}

NuvashaTaskScope *
nuvasha_modrinth_details_async (const char *project_id, GAsyncReadyCallback callback,
                            gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);

  {
    g_autofree char *escaped_id =
      g_uri_escape_string (project_id, NULL, FALSE);

    url = g_strdup_printf (nuvasha_MODRINTH_URL "/project/%s", escaped_id);
  }
  return get_async (url, callback, user_data, details_answer,
                    nuvasha_NOT_FOUND_IS_ERROR);
}
NuvashaModrinthDetails *
nuvasha_modrinth_details_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The versions of a project
 * ---------------------------------------------------------- */

/* Instance context used to validate a request's response. */
typedef struct {
  NuvashaContentType type;
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
instance_ask_new (NuvashaContentType type, const char *minecraft_version,
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

  versions = nuvasha_modrinth_versions_from_json (json, -1, asked->type,
                                              asked->minecraft_version,
                                              asked->loader, &error);

  if (versions == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, versions, (GDestroyNotify) g_ptr_array_unref);
}

NuvashaTaskScope *
nuvasha_modrinth_versions_async (const char *project_id, NuvashaContentType type,
                             const char *minecraft_version, const char *loader,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaTaskScope) scope = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *escaped_id = NULL;
  g_autofree char *escaped_game = NULL;
  g_autofree char *escaped_loaders = NULL;
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  game_versions = nuvasha_modrinth_string_array_json (minecraft_version);
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
  if (nuvasha_content_type_needs_loader (type) && loader != NULL &&
      *loader != '\0')
    {
      loaders = nuvasha_modrinth_loaders_json (loader);
      escaped_loaders = g_uri_escape_string (loaders, NULL, FALSE);
    }

  if (escaped_loaders != NULL)
    url = g_strdup_printf (
        nuvasha_MODRINTH_URL "/project/%s/version?game_versions=%s&loaders=%s",
        escaped_id, escaped_game, escaped_loaders);
  else
    url = g_strdup_printf (
        nuvasha_MODRINTH_URL "/project/%s/version?game_versions=%s",
        escaped_id, escaped_game);

  scope = request_new (callback, user_data, versions_answer,
                       nuvasha_NOT_FOUND_IS_ERROR,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  nuvasha_task_scope_release_when_done (
    nuvasha_download_json_cached_async (nuvasha_downloads_get_service (), url, json_ready,
                                    nuvasha_task_scope_task (scope)),
    nuvasha_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvasha_modrinth_versions_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
categories_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  Request *request = g_task_get_task_data (task);
  GPtrArray *categories = NULL;

  categories = nuvasha_modrinth_categories_from_json (
      json, -1, (const char *) request->data, &error);

  if (categories == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, categories, (GDestroyNotify) g_ptr_array_unref);
}

NuvashaTaskScope *
nuvasha_modrinth_categories_async (const char        *project_type,
                               GAsyncReadyCallback callback,
                               gpointer           user_data)
{
  g_autofree char *url = g_strdup_printf (nuvasha_MODRINTH_URL "/tag/category");
  g_autoptr (NuvashaTaskScope) scope = NULL;

  scope = request_new (callback, user_data, categories_answer,
                       nuvasha_NOT_FOUND_IS_ERROR, g_strdup (project_type), g_free);
  nuvasha_task_scope_release_when_done (
    nuvasha_download_json_cached_async (nuvasha_downloads_get_service (), url, json_ready,
                                    nuvasha_task_scope_task (scope)),
    nuvasha_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvasha_modrinth_categories_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

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
  NuvashaModrinthVersion *version = NULL;

  version = nuvasha_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, version,
                         (GDestroyNotify) nuvasha_modrinth_version_free);
}

NuvashaTaskScope *
nuvasha_modrinth_version_by_hash_async (const char *sha1,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (nuvasha_MODRINTH_URL "/version_file/%s?algorithm=sha1",
                           escaped_sha1);
  }
  return get_async (url, callback, user_data, version_answer,
                    nuvasha_NOT_FOUND_IS_ERROR);
}

NuvashaModrinthVersion *
nuvasha_modrinth_version_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

NuvashaTaskScope *
nuvasha_modrinth_version_by_id_async (const char *version_id,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  g_autofree char *escaped = g_uri_escape_string (version_id, NULL, FALSE);
  g_autofree char *url = g_strdup_printf (nuvasha_MODRINTH_URL "/version/%s",
                                          escaped);

  return get_async (url, callback, user_data, version_answer,
                    nuvasha_NOT_FOUND_IS_ERROR);
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
  NuvashaModrinthVersion *version = NULL;

  version = nuvasha_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* Modrinth was asked for something that fits this instance, so
   * anything else it answered is not an update this instance can take. */
  if (!nuvasha_modrinth_version_fits (version, asked->type,
                                  asked->minecraft_version, asked->loader))
    {
      nuvasha_modrinth_version_free (version);
      request_return_pointer (task, NULL, NULL);
      return;
    }

  request_return_pointer (task, version,
                         (GDestroyNotify) nuvasha_modrinth_version_free);
}

NuvashaTaskScope *
nuvasha_modrinth_update_by_hash_async (NuvashaContentType type, const char *sha1,
                                   const char *minecraft_version,
                                   const char *loader,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_autoptr (NuvashaTaskScope) scope = NULL;
  g_autofree char *url = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *body = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  /* Both narrowings are already JSON arrays, so they can be inserted directly
   * into the request body. */
  game_versions = nuvasha_modrinth_string_array_json (minecraft_version);
  loaders = nuvasha_modrinth_string_array_json (
    nuvasha_content_type_needs_loader (type) && loader != NULL ? loader
                                                            : "minecraft");

  body = g_strdup_printf ("{\"game_versions\":%s,\"loaders\":%s}",
                          game_versions, loaders);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (
        nuvasha_MODRINTH_URL "/version_file/%s/update?algorithm=sha1",
        escaped_sha1);
  }

  scope = request_new (callback, user_data, update_answer,
                       nuvasha_NOT_FOUND_IS_ABSENT,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  nuvasha_task_scope_release_when_done (
    nuvasha_download_json_post_cached_async (nuvasha_downloads_get_service (), url, body, nuvasha_MODRINTH_USER_AGENT,
                                         json_ready,
                                         nuvasha_task_scope_task (scope)),
    nuvasha_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

NuvashaModrinthVersion *
nuvasha_modrinth_update_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Dependencies required to install a version
 * ---------------------------------------------------------- */

char *
nuvasha_modrinth_no_compatible_message (NuvashaContentType type,
                                    const char *minecraft_version,
                                    const char *loader)
{
  const char *version = minecraft_version != NULL ? minecraft_version : "";

  if (nuvasha_content_type_needs_loader (type))
    return g_strdup_printf (_("No compatible version found for Minecraft %s and %s."),
                            version, loader != NULL ? loader : "");

  return g_strdup_printf (_("No compatible version found for Minecraft %s."),
                          version);
}

GPtrArray *
nuvasha_modrinth_resolve_install (NuvashaModrinthVersion *version,
                              GHashTable *installed, GPtrArray **conflicts,
                              GError **error)
{
  GPtrArray *pending = NULL;

  g_return_val_if_fail (version != NULL, NULL);

  if (conflicts != NULL)
    *conflicts = g_ptr_array_new_with_free_func (
        (GDestroyNotify) nuvasha_modrinth_dependency_free);

  if (version->dependencies == NULL)
    return g_ptr_array_new_with_free_func (
        (GDestroyNotify) nuvasha_modrinth_dependency_free);

  /* Check conflicts only against content installed in this instance. */
  if (conflicts != NULL)
    {
      for (guint i = 0; i < version->dependencies->len; i++)
        {
          NuvashaModrinthDependency *dependency =
            g_ptr_array_index (version->dependencies, i);

          if (dependency->type != nuvasha_DEPENDENCY_INCOMPATIBLE ||
              dependency->project_id == NULL || installed == NULL)
            continue;

          if (!g_hash_table_contains (installed, dependency->project_id))
            continue;

          g_ptr_array_add (*conflicts, nuvasha_modrinth_dependency_copy (dependency));
        }
    }

  pending = g_ptr_array_new_with_free_func (
      (GDestroyNotify) nuvasha_modrinth_dependency_free);

  for (guint i = 0; i < version->dependencies->len; i++)
    {
      NuvashaModrinthDependency *dependency =
        g_ptr_array_index (version->dependencies, i);
      gboolean already = FALSE;

      /* Embedded is inside the file, optional is a choice and
       * incompatible is a conflict: only dependencies the version cannot run
       * without is fetched. */
      if (dependency->type != nuvasha_DEPENDENCY_REQUIRED)
        continue;

      /* A dependency that names no project names nothing that could be
       * fetched, so the install cannot go on as if it did not exist. */
      if (dependency->project_id == NULL)
        {
          g_set_error_literal (error, nuvasha_MODRINTH_ERROR,
                               nuvasha_MODRINTH_ERROR_PARSE,
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
          NuvashaModrinthDependency *one = g_ptr_array_index (pending, j);

          already = g_strcmp0 (one->project_id, dependency->project_id) == 0;
        }

      if (already)
        continue;

      g_ptr_array_add (pending, nuvasha_modrinth_dependency_copy (dependency));
    }

  return pending;
}
