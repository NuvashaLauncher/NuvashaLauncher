/* Modrinth request scheduling, deduplication, and error handling. */

#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-modrinth-private.h"
#include "nuvashalauncher-task-private.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-i18n.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * The requests
 * ---------------------------------------------------------- */

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

void
nuvashalauncher_modrinth_request_return_error (GTask *task, GError *error)
{
  Request *request = g_task_get_task_data (task);
  nuvashalauncher_task_scope_return_error (request->scope, error);
}

void
nuvashalauncher_modrinth_request_return_pointer (GTask *task, gpointer value, GDestroyNotify free_value)
{
  Request *request = g_task_get_task_data (task);
  nuvashalauncher_task_scope_return_pointer (request->scope, value, free_value);
}

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_request_new (GAsyncReadyCallback callback, gpointer user_data,
             AnswerFunc answer, nuvashalauncherNotFoundRule not_found, void *data,
             GDestroyNotify free_data)
{
  nuvashalauncherTaskScope *scope = nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvashalauncher_task_scope_task (scope);
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

/* Return an error in this module's domain while preserving the nuvashalauncher_modrinth_failed URL
 * and download error details. */
void
nuvashalauncher_modrinth_fail (GTask *task, GError *error)
{
  Request *request = g_task_get_task_data (task);
  GError *wrapped = NULL;

  if (g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
      g_clear_error (&error);

      nuvashalauncher_task_scope_return_error (
        request->scope,
        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "the request was cancelled"));
      return;
    }

  if (error->domain == nuvashalauncher_DOWNLOAD_ERROR)
    {
      if (error->code == nuvashalauncher_DOWNLOAD_ERROR_RATE_LIMITED)
        wrapped = g_error_new_literal (
            nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_RATE_LIMIT,
            _("Modrinth is asking Nuvasha Launcher to slow down, too many requests"));
      else if (error->code == nuvashalauncher_DOWNLOAD_ERROR_HTTP)
        wrapped = g_error_new (nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_HTTP,
                               "%s", error->message);
      else if (error->code == nuvashalauncher_DOWNLOAD_ERROR_INVALID)
        wrapped = g_error_new (nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_PARSE,
                               "%s", error->message);
      else if (error->code == nuvashalauncher_DOWNLOAD_ERROR_IO ||
               error->code == nuvashalauncher_DOWNLOAD_ERROR_CANCELLED)
        wrapped = g_steal_pointer (&error);
      else
        wrapped = g_error_new (nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_NETWORK,
                               "%s", error->message);
    }
  else
    wrapped = g_error_new (nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_PARSE,
                           "%s", error->message);

  g_clear_error (&error);
  nuvashalauncher_task_scope_return_error (request->scope, wrapped);
}

/* Check whether a download error represents HTTP 404. */
gboolean
nuvashalauncher_modrinth_error_is_not_found (const GError *error)
{
  return error->domain == nuvashalauncher_DOWNLOAD_ERROR &&
         error->code == nuvashalauncher_DOWNLOAD_ERROR_HTTP &&
         strstr (error->message, "HTTP 404") != NULL;
}

static void
json_flight_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  JsonFlight *flight = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = nuvashalauncher_download_json_finish (result, &error);
  JsonNode *root = parser != NULL ? json_parser_get_root (parser) : NULL;

  /* Remove before delivering callbacks: a callback may immediately request
   * the same URL again, and that request must start a fresh flight. */
  g_hash_table_steal (json_flights, flight->key);

  for (guint i = 0; i < flight->tasks->len; i++)
    {
      GTask *task = g_ptr_array_index (flight->tasks, i);
      Request *request = g_task_get_task_data (task);

      if (nuvashalauncher_task_scope_return_error_if_cancelled (request->scope))
        continue;

      if (parser == NULL)
        {
          if (nuvashalauncher_modrinth_error_is_not_found (error) &&
              request->not_found == nuvashalauncher_NOT_FOUND_IS_ABSENT)
            {
               nuvashalauncher_task_scope_return_pointer (request->scope, NULL, NULL);
              continue;
            }

          if (nuvashalauncher_modrinth_error_is_not_found (error))
            {
               nuvashalauncher_task_scope_return_error (
                 request->scope,
                 g_error_new_literal (nuvashalauncher_MODRINTH_ERROR,
                                      nuvashalauncher_MODRINTH_ERROR_NOT_FOUND,
                                       _("Modrinth does not know this file")));
              continue;
            }

          nuvashalauncher_modrinth_fail (task, g_error_copy (error));
          continue;
        }

      if (root == NULL)
        {
           nuvashalauncher_task_scope_return_error (
             request->scope,
             g_error_new_literal (nuvashalauncher_MODRINTH_ERROR,
                                  nuvashalauncher_MODRINTH_ERROR_PARSE,
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

void
nuvashalauncher_modrinth_json_ready (GObject *source G_GNUC_UNUSED,
                                     GAsyncResult *result, gpointer user_data)
{
  /* The flight stores this task as callback data without owning another
     reference. Keep one while this callback uses the task, but do not treat
     the borrowed callback pointer itself as an owned reference. */
  g_autoptr (GTask) task = g_object_ref (user_data);
  Request *request = g_task_get_task_data (task);
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) error = NULL;
  JsonNode *root = NULL;

  parser = nuvashalauncher_download_json_finish (result, &error);

  if (parser == NULL)
    {
      if (nuvashalauncher_modrinth_error_is_not_found (error) &&
          request->not_found == nuvashalauncher_NOT_FOUND_IS_ABSENT)
        {
          g_clear_error (&error);
          nuvashalauncher_modrinth_request_return_pointer (task, NULL, NULL);
          return;
        }

      if (nuvashalauncher_modrinth_error_is_not_found (error))
        {
          g_clear_error (&error);
          nuvashalauncher_modrinth_request_return_error (
            task,
            g_error_new_literal (nuvashalauncher_MODRINTH_ERROR,
                                 nuvashalauncher_MODRINTH_ERROR_NOT_FOUND,
                                  _("Modrinth does not know this file")));
          return;
        }

      nuvashalauncher_modrinth_fail (task, g_steal_pointer (&error));
      return;
    }

  root = json_parser_get_root (parser);

  if (root == NULL)
    {
      nuvashalauncher_modrinth_request_return_error (
        task,
        g_error_new_literal (nuvashalauncher_MODRINTH_ERROR, nuvashalauncher_MODRINTH_ERROR_PARSE,
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

nuvashalauncherTaskScope *
nuvashalauncher_modrinth_get_async (const char *url, GAsyncReadyCallback callback, gpointer user_data,
           AnswerFunc answer, nuvashalauncherNotFoundRule not_found)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_modrinth_request_new (callback, user_data, answer, not_found, NULL, NULL);
  GTask *task = nuvashalauncher_task_scope_task (scope);
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
  /* The flight owns its key. Stealing the entry before callbacks lets the
   * flight be freed immediately; using the same allocation for the table key
   * ensures that ownership is released exactly once. */
  g_hash_table_insert (json_flights, flight->key, flight);

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_json_cached_async (nuvashalauncher_downloads_get_service (), url, json_flight_done, flight),
    nuvashalauncher_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}
