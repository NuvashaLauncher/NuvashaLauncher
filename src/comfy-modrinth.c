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
 * comfy_modrinth_version_fits(), rather than a copy of it per caller.
 *
 * Every function is called from the thread-default main context that
 * answers it, normally the GTK main loop, and nothing here blocks it:
 * the requests are the download module's, and the only work done on the
 * main loop is parsing a body that has already arrived. */

#include "comfy-modrinth.h"
#include "comfy-modrinth-private.h"
#include "comfy-task-private.h"

#include "comfy-download.h"
#include "comfy-downloads.h"
#include "comfy-i18n.h"
#include "comfy-json.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * The requests
 * ---------------------------------------------------------- */

/* What a request hands its parsed answer to. */
typedef void (*AnswerFunc) (GTask *task, JsonNode *root);

/* What an HTTP 404 means to a request: for an update check it is the
 * answer that nothing is newer, for a hash lookup it says the file is
 * not one Modrinth published. */
typedef enum {
  comfy_NOT_FOUND_IS_ERROR, /* the answer came from a known place */
  comfy_NOT_FOUND_IS_ABSENT /* nothing there is an answer, not a failure */
} ComfyNotFoundRule;

typedef struct {
  AnswerFunc answer;
  ComfyNotFoundRule not_found;
  ComfyTaskScope *scope;
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
  comfy_task_scope_return_error (request->scope, error);
}

static void
request_return_pointer (GTask *task, gpointer value, GDestroyNotify free_value)
{
  Request *request = g_task_get_task_data (task);
  comfy_task_scope_return_pointer (request->scope, value, free_value);
}

static ComfyTaskScope *
request_new (GAsyncReadyCallback callback, gpointer user_data,
             AnswerFunc answer, ComfyNotFoundRule not_found, void *data,
             GDestroyNotify free_data)
{
  ComfyTaskScope *scope = comfy_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = comfy_task_scope_task (scope);
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
 * caller can tell an answer comfy cannot use from a network that is not
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

      comfy_task_scope_return_error (
        request->scope,
        g_error_new_literal (comfy_MODRINTH_ERROR,
                             comfy_MODRINTH_ERROR_NETWORK,
                             "the request was cancelled"));
      return;
    }

  if (error->domain == comfy_DOWNLOAD_ERROR)
    {
      if (error->code == comfy_DOWNLOAD_ERROR_RATE_LIMITED)
        wrapped = g_error_new_literal (
            comfy_MODRINTH_ERROR, comfy_MODRINTH_ERROR_RATE_LIMIT,
            "Modrinth is asking comfy to slow down, too many requests");
      else if (error->code == comfy_DOWNLOAD_ERROR_HTTP)
        wrapped = g_error_new (comfy_MODRINTH_ERROR, comfy_MODRINTH_ERROR_HTTP,
                               "%s", error->message);
      else
        wrapped = g_error_new (comfy_MODRINTH_ERROR, comfy_MODRINTH_ERROR_NETWORK,
                               "%s", error->message);
    }
  else
    wrapped = g_error_new (comfy_MODRINTH_ERROR, comfy_MODRINTH_ERROR_PARSE,
                           "%s", error->message);

  g_error_free (error);
  comfy_task_scope_return_error (request->scope, wrapped);
}

/* The download module writes the status into its own message, which is
 * the one place the code of an HTTP error lives. */
static gboolean
error_is_not_found (const GError *error)
{
  return error->domain == comfy_DOWNLOAD_ERROR &&
         error->code == comfy_DOWNLOAD_ERROR_HTTP &&
         strstr (error->message, "HTTP 404") != NULL;
}

static void
json_flight_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  JsonFlight *flight = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = comfy_download_json_finish (result, &error);
  JsonNode *root = parser != NULL ? json_parser_get_root (parser) : NULL;

  /* Remove before delivering callbacks: a callback may immediately request
   * the same URL again, and that request must start a fresh flight. */
  g_hash_table_steal (json_flights, flight->key);

  for (guint i = 0; i < flight->tasks->len; i++)
    {
      GTask *task = g_ptr_array_index (flight->tasks, i);
      Request *request = g_task_get_task_data (task);

      if (comfy_task_scope_return_error_if_cancelled (request->scope))
        continue;

      if (parser == NULL)
        {
          if (error_is_not_found (error) &&
              request->not_found == comfy_NOT_FOUND_IS_ABSENT)
            {
               comfy_task_scope_return_pointer (request->scope, NULL, NULL);
              continue;
            }

          if (error_is_not_found (error))
            {
               comfy_task_scope_return_error (
                 request->scope,
                 g_error_new_literal (comfy_MODRINTH_ERROR,
                                      comfy_MODRINTH_ERROR_NOT_FOUND,
                                      "Modrinth does not know this file"));
              continue;
            }

          fail (task, g_error_copy (error));
          continue;
        }

      if (root == NULL)
        {
           comfy_task_scope_return_error (
             request->scope,
             g_error_new_literal (comfy_MODRINTH_ERROR,
                                  comfy_MODRINTH_ERROR_PARSE,
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

  parser = comfy_download_json_finish (result, &error);

  if (parser == NULL)
    {
      if (error_is_not_found (error) &&
          request->not_found == comfy_NOT_FOUND_IS_ABSENT)
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
            g_error_new_literal (comfy_MODRINTH_ERROR,
                                 comfy_MODRINTH_ERROR_NOT_FOUND,
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
        g_error_new_literal (comfy_MODRINTH_ERROR, comfy_MODRINTH_ERROR_PARSE,
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

static ComfyTaskScope *
get_async (const char *url, GAsyncReadyCallback callback, gpointer user_data,
           AnswerFunc answer, ComfyNotFoundRule not_found)
{
  g_autoptr (ComfyTaskScope) scope =
    request_new (callback, user_data, answer, not_found, NULL, NULL);
  GTask *task = comfy_task_scope_task (scope);
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

  comfy_task_scope_release_when_done (
    comfy_download_json_cached_async (comfy_downloads_get_service (), url, json_flight_done, flight),
    comfy_task_scope_get_cancellable (scope));

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
  ComfyModrinthResults *results = NULL;

  results = comfy_modrinth_results_from_json (json, -1, &error);

  if (results == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, results,
                         (GDestroyNotify) comfy_modrinth_results_free);
}

ComfyTaskScope *
comfy_modrinth_search_async (const ComfyModrinthSearch *search,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (search != NULL, NULL);

  url = comfy_modrinth_search_url (search);
  return get_async (url, callback, user_data, search_answer,
                    comfy_NOT_FOUND_IS_ERROR);
}

ComfyModrinthResults *
comfy_modrinth_search_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
details_answer (GTask *task, JsonNode *root)
{
  g_autofree char *json = json_to_string (root, FALSE);
  g_autoptr (GError) error = NULL;
  ComfyModrinthDetails *details = NULL;

  details = comfy_modrinth_details_from_json (json, -1, &error);

  if (details == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, details,
                         (GDestroyNotify) comfy_modrinth_details_free);
}

ComfyTaskScope *
comfy_modrinth_details_async (const char *project_id, GAsyncReadyCallback callback,
                            gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);

  {
    g_autofree char *escaped_id =
      g_uri_escape_string (project_id, NULL, FALSE);

    url = g_strdup_printf (comfy_MODRINTH_URL "/project/%s", escaped_id);
  }
  return get_async (url, callback, user_data, details_answer,
                    comfy_NOT_FOUND_IS_ERROR);
}
ComfyModrinthDetails *
comfy_modrinth_details_finish (GAsyncResult *result, GError **error)
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
  ComfyContentType type;
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
instance_ask_new (ComfyContentType type, const char *minecraft_version,
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

  versions = comfy_modrinth_versions_from_json (json, -1, asked->type,
                                              asked->minecraft_version,
                                              asked->loader, &error);

  if (versions == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, versions, (GDestroyNotify) g_ptr_array_unref);
}

ComfyTaskScope *
comfy_modrinth_versions_async (const char *project_id, ComfyContentType type,
                             const char *minecraft_version, const char *loader,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (ComfyTaskScope) scope = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *escaped_id = NULL;
  g_autofree char *escaped_game = NULL;
  g_autofree char *escaped_loaders = NULL;
  g_autofree char *url = NULL;

  g_return_val_if_fail (project_id != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  game_versions = comfy_modrinth_string_array_json (minecraft_version);
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
  if (comfy_content_type_needs_loader (type) && loader != NULL &&
      *loader != '\0')
    {
      loaders = comfy_modrinth_loaders_json (loader);
      escaped_loaders = g_uri_escape_string (loaders, NULL, FALSE);
    }

  if (escaped_loaders != NULL)
    url = g_strdup_printf (
        comfy_MODRINTH_URL "/project/%s/version?game_versions=%s&loaders=%s",
        escaped_id, escaped_game, escaped_loaders);
  else
    url = g_strdup_printf (
        comfy_MODRINTH_URL "/project/%s/version?game_versions=%s",
        escaped_id, escaped_game);

  scope = request_new (callback, user_data, versions_answer,
                       comfy_NOT_FOUND_IS_ERROR,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  comfy_task_scope_release_when_done (
    comfy_download_json_cached_async (comfy_downloads_get_service (), url, json_ready,
                                    comfy_task_scope_task (scope)),
    comfy_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
comfy_modrinth_versions_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The categories a search can be narrowed by
 * ---------------------------------------------------------- */

void
comfy_modrinth_category_free (ComfyModrinthCategory *category)
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
comfy_modrinth_category_label (const char *name)
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
comfy_modrinth_project_type_for_content (ComfyContentType type)
{
  return g_strdup (comfy_content_type_to_string (type));
}

static int
compare_by_name (gconstpointer a, gconstpointer b)
{
  const ComfyModrinthCategory *left = *(ComfyModrinthCategory *const *) a;
  const ComfyModrinthCategory *right = *(ComfyModrinthCategory *const *) b;

  return g_strcmp0 (left->name, right->name);
}

GPtrArray *
comfy_modrinth_categories_from_json (const char *json, gssize length,
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
      g_set_error_literal (error, comfy_MODRINTH_ERROR,
                           comfy_MODRINTH_ERROR_PARSE,
                           "the categories answer is not a list");
      return NULL;
    }

  categories =
    g_ptr_array_new_with_free_func ((GDestroyNotify) comfy_modrinth_category_free);
  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      g_autofree char *item_type = NULL;
      g_autofree char *name = NULL;
      g_autofree char *header = NULL;
      ComfyModrinthCategory *category = NULL;
      JsonNode *item = json_array_get_element (array, i);

      if (comfy_json_object_of (item) == NULL)
        continue;

      /* One project type publishes one list of categories, so the other
       * kinds' categories are not offered here. This is also what keeps a
       * pack page from offering "fabric". */
      item_type = comfy_json_string (item, "project_type");
      name = comfy_json_string (item, "name");

      if (name == NULL || g_strcmp0 (item_type, project_type) != 0)
        continue;

      header = comfy_json_string (item, "header");

      category = g_new0 (ComfyModrinthCategory, 1);
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

  categories = comfy_modrinth_categories_from_json (
      json, -1, (const char *) request->data, &error);

  if (categories == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, categories, (GDestroyNotify) g_ptr_array_unref);
}

ComfyTaskScope *
comfy_modrinth_categories_async (const char        *project_type,
                               GAsyncReadyCallback callback,
                               gpointer           user_data)
{
  g_autofree char *url = g_strdup_printf (comfy_MODRINTH_URL "/tag/category");
  g_autoptr (ComfyTaskScope) scope = NULL;

  scope = request_new (callback, user_data, categories_answer,
                       comfy_NOT_FOUND_IS_ERROR, g_strdup (project_type), g_free);
  comfy_task_scope_release_when_done (
    comfy_download_json_cached_async (comfy_downloads_get_service (), url, json_ready,
                                    comfy_task_scope_task (scope)),
    comfy_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
comfy_modrinth_categories_finish (GAsyncResult *result, GError **error)
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
  ComfyModrinthVersion *version = NULL;

  version = comfy_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  request_return_pointer (task, version,
                         (GDestroyNotify) comfy_modrinth_version_free);
}

ComfyTaskScope *
comfy_modrinth_version_by_hash_async (const char *sha1,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_autofree char *url = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (comfy_MODRINTH_URL "/version_file/%s?algorithm=sha1",
                           escaped_sha1);
  }
  return get_async (url, callback, user_data, version_answer,
                    comfy_NOT_FOUND_IS_ERROR);
}

ComfyModrinthVersion *
comfy_modrinth_version_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

ComfyTaskScope *
comfy_modrinth_version_by_id_async (const char *version_id,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  g_autofree char *escaped = g_uri_escape_string (version_id, NULL, FALSE);
  g_autofree char *url = g_strdup_printf (comfy_MODRINTH_URL "/version/%s",
                                          escaped);

  return get_async (url, callback, user_data, version_answer,
                    comfy_NOT_FOUND_IS_ERROR);
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
  ComfyModrinthVersion *version = NULL;

  version = comfy_modrinth_version_from_json (json, -1, &error);

  if (version == NULL)
    {
      request_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* Modrinth was asked for something that fits this instance, so
   * anything else it answered is not an update this instance can take. */
  if (!comfy_modrinth_version_fits (version, asked->type,
                                  asked->minecraft_version, asked->loader))
    {
      comfy_modrinth_version_free (version);
      request_return_pointer (task, NULL, NULL);
      return;
    }

  request_return_pointer (task, version,
                         (GDestroyNotify) comfy_modrinth_version_free);
}

ComfyTaskScope *
comfy_modrinth_update_by_hash_async (ComfyContentType type, const char *sha1,
                                   const char *minecraft_version,
                                   const char *loader,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_autoptr (ComfyTaskScope) scope = NULL;
  g_autofree char *url = NULL;
  g_autofree char *game_versions = NULL;
  g_autofree char *loaders = NULL;
  g_autofree char *body = NULL;

  g_return_val_if_fail (sha1 != NULL, NULL);
  g_return_val_if_fail (minecraft_version != NULL, NULL);

  /* Both narrowings are already JSON arrays, so the body is written
   * around them rather than through a builder. */
  game_versions = comfy_modrinth_string_array_json (minecraft_version);
  loaders = comfy_modrinth_string_array_json (
    comfy_content_type_needs_loader (type) && loader != NULL ? loader
                                                            : "minecraft");

  body = g_strdup_printf ("{\"game_versions\":%s,\"loaders\":%s}",
                          game_versions, loaders);

  {
    g_autofree char *escaped_sha1 =
      g_uri_escape_string (sha1, NULL, FALSE);

    url = g_strdup_printf (
        comfy_MODRINTH_URL "/version_file/%s/update?algorithm=sha1",
        escaped_sha1);
  }

  scope = request_new (callback, user_data, update_answer,
                       comfy_NOT_FOUND_IS_ABSENT,
                       instance_ask_new (type, minecraft_version, loader),
                       instance_ask_free);

  comfy_task_scope_release_when_done (
    comfy_download_json_post_cached_async (comfy_downloads_get_service (), url, body, comfy_MODRINTH_USER_AGENT,
                                         json_ready,
                                         comfy_task_scope_task (scope)),
    comfy_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

ComfyModrinthVersion *
comfy_modrinth_update_by_hash_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * What an install of a version needs
 * ---------------------------------------------------------- */

char *
comfy_modrinth_no_compatible_message (ComfyContentType type,
                                    const char *minecraft_version,
                                    const char *loader)
{
  const char *version = minecraft_version != NULL ? minecraft_version : "";

  if (comfy_content_type_needs_loader (type))
    return g_strdup_printf (_("No compatible version found for Minecraft %s and %s."),
                            version, loader != NULL ? loader : "");

  return g_strdup_printf (_("No compatible version found for Minecraft %s."),
                          version);
}

GPtrArray *
comfy_modrinth_resolve_install (ComfyModrinthVersion *version,
                              GHashTable *installed, GPtrArray **conflicts,
                              GError **error)
{
  GPtrArray *pending = NULL;

  g_return_val_if_fail (version != NULL, NULL);

  if (conflicts != NULL)
    *conflicts = g_ptr_array_new_with_free_func (
        (GDestroyNotify) comfy_modrinth_dependency_free);

  if (version->dependencies == NULL)
    return g_ptr_array_new_with_free_func (
        (GDestroyNotify) comfy_modrinth_dependency_free);

  /* What @version says it does not run with, but only for what is
   * actually installed: a conflict with something that is not there is
   * nothing to decide about. */
  if (conflicts != NULL)
    {
      for (guint i = 0; i < version->dependencies->len; i++)
        {
          ComfyModrinthDependency *dependency =
            g_ptr_array_index (version->dependencies, i);

          if (dependency->type != comfy_DEPENDENCY_INCOMPATIBLE ||
              dependency->project_id == NULL || installed == NULL)
            continue;

          if (!g_hash_table_contains (installed, dependency->project_id))
            continue;

          g_ptr_array_add (*conflicts, comfy_modrinth_dependency_copy (dependency));
        }
    }

  pending = g_ptr_array_new_with_free_func (
      (GDestroyNotify) comfy_modrinth_dependency_free);

  for (guint i = 0; i < version->dependencies->len; i++)
    {
      ComfyModrinthDependency *dependency =
        g_ptr_array_index (version->dependencies, i);
      gboolean already = FALSE;

      /* Embedded is inside the file, optional is a choice and
       * incompatible is a conflict: only what the version cannot run
       * without is fetched. */
      if (dependency->type != comfy_DEPENDENCY_REQUIRED)
        continue;

      /* A dependency that names no project names nothing that could be
       * fetched, so the install cannot go on as if it did not exist. */
      if (dependency->project_id == NULL)
        {
          g_set_error_literal (error, comfy_MODRINTH_ERROR,
                               comfy_MODRINTH_ERROR_PARSE,
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
          ComfyModrinthDependency *one = g_ptr_array_index (pending, j);

          already = g_strcmp0 (one->project_id, dependency->project_id) == 0;
        }

      if (already)
        continue;

      g_ptr_array_add (pending, comfy_modrinth_dependency_copy (dependency));
    }

  return pending;
}
