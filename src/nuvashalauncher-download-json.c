/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Download JSON response handling. */

#include "nuvashalauncher-download-private.h"

typedef struct {
  GBytes *body;
  char *stale_json;
  gboolean is_not_modified;
  gboolean encode_cache;
  char *etag;
  GError *status_error;
  JsonParser *parser;
  GError *parse_error;
  char *cache_data;
  gsize cache_len;
} JsonProcessTask;

static void
json_process_task_free (JsonProcessTask *state)
{
  if (state == NULL)
    return;

  g_clear_pointer (&state->body, g_bytes_unref);
  g_free (state->stale_json);
  g_free (state->etag);
  g_clear_error (&state->status_error);
  g_clear_object (&state->parser);
  g_clear_error (&state->parse_error);
  g_free (state->cache_data);
  g_free (state);
}

static void
json_process_body_worker (GTask *task,
                          gpointer source_object G_GNUC_UNUSED,
                          gpointer task_data,
                          GCancellable *cancellable G_GNUC_UNUSED)
{
  JsonProcessTask *state = task_data;

  if (g_task_return_error_if_cancelled (task))
    return;

  if (state->is_not_modified)
    {
      g_autoptr (JsonParser) stale = json_parser_new ();

      if (json_parser_load_from_data (stale, state->stale_json, -1, NULL) &&
          json_parser_get_root (stale) != NULL)
        {
          state->parser = g_steal_pointer (&stale);
          if (state->encode_cache)
            state->cache_data = nuvashalauncher_download_cache_encode (
              state->stale_json, state->etag, &state->cache_len);
        }
      g_clear_pointer (&state->stale_json, g_free);
      g_task_return_boolean (task, TRUE);
      return;
    }

  {
    gsize size = 0;
    const char *data = state->body != NULL
                         ? g_bytes_get_data (state->body, &size)
                         : NULL;
    g_autoptr (JsonParser) parser = json_parser_new ();
    g_autoptr (GError) parse_error = NULL;

    if (!json_parser_load_from_data (parser, data != NULL ? data : "",
                                     (gssize) size, &parse_error))
      {
        /* A body that is not JSON would come back unchanged on a retry,
         * so the parse error is reported as it is. */
        state->parse_error = g_steal_pointer (&parse_error);
      }
    else if (json_parser_get_root (parser) == NULL)
      {
        /* json-glib accepts an empty document without complaint; there
         * is nothing to hand to a caller though. */
        state->parse_error = g_error_new_literal (
          JSON_PARSER_ERROR, JSON_PARSER_ERROR_PARSE,
          "the response body holds no JSON");
      }
    else
      {
        state->parser = g_steal_pointer (&parser);
        if (state->encode_cache && data != NULL)
          {
            g_autofree char *response = g_strndup (data, size);
            state->cache_data = nuvashalauncher_download_cache_encode (
              response, state->etag, &state->cache_len);
          }
      }
    g_clear_pointer (&state->body, g_bytes_unref);
  }

  g_task_return_boolean (task, TRUE);
}

static void
json_process_body_ready (GObject *source G_GNUC_UNUSED,
                         GAsyncResult *result,
                         gpointer user_data)
{
  DownloadRequest *request = user_data;
  GTask *task = G_TASK (result);
  g_autoptr (GError) error = NULL;
  JsonProcessTask *state = g_task_get_task_data (task);

  g_task_propagate_boolean (task, &error);

  if (request_was_cancelled (request, error))
    {
      nuvashalauncher_download_request_fail (
        request, nuvashalauncher_download_cancelled_error ());
      return;
    }

  if (state->is_not_modified)
    {
      if (state->parser != NULL)
        {
          g_clear_error (&state->status_error);

          /* The server confirmed the cached copy: renew its timestamp so it
             is not revalidated on every request. */
          if (request->api_cache && request->cache_path != NULL &&
              state->cache_data != NULL)
            {
              g_clear_pointer (&request->response_etag, g_free);
              request->response_etag = g_strdup (request->cache_etag);
              request->pending_parser = g_steal_pointer (&state->parser);
              nuvashalauncher_download_cache_write_encoded (
                request, g_steal_pointer (&state->cache_data), state->cache_len);
              return;
            }

          nuvashalauncher_download_request_succeed_json (
            request, g_steal_pointer (&state->parser));
          return;
        }

      end_attempt (request, g_steal_pointer (&state->status_error));
      return;
    }

  if (state->parse_error != NULL)
    {
      nuvashalauncher_download_request_fail (
        request, g_steal_pointer (&state->parse_error));
      return;
    }

  if (request->api_cache && request->cache_path != NULL &&
      state->cache_data != NULL)
    {
      request->pending_parser = g_steal_pointer (&state->parser);
      nuvashalauncher_download_cache_write_encoded (
        request, g_steal_pointer (&state->cache_data), state->cache_len);
      return;
    }

  nuvashalauncher_download_request_succeed_json (
    request, g_steal_pointer (&state->parser));
}

void
nuvashalauncher_download_json_process_body (DownloadRequest *request, GBytes *body)
{
  GError *status_error = check_status (request);
  guint status = soup_message_get_status (request->message);
  gboolean is_not_modified =
    request->stale_json != NULL && status == SOUP_STATUS_NOT_MODIFIED;
  gboolean encode_cache = request->api_cache && request->cache_path != NULL;
  JsonProcessTask *state = NULL;
  g_autoptr (GTask) parse_task = NULL;

  if (!is_not_modified)
    {
      if (status_error != NULL)
        {
          end_attempt (request, status_error);
          return;
        }

      {
        const char *etag = soup_message_headers_get_one (
          soup_message_get_response_headers (request->message), "ETag");

        g_clear_pointer (&request->response_etag, g_free);
        request->response_etag = g_strdup (etag);
      }

      {
        gsize size = 0;

        if (body != NULL)
          g_bytes_get_data (body, &size);

        if (size > MAX_JSON_RESPONSE_SIZE)
          {
            nuvashalauncher_download_request_fail (
              request,
              g_error_new (NUVASHALAUNCHER_DOWNLOAD_ERROR,
                           NUVASHALAUNCHER_DOWNLOAD_ERROR_INVALID,
                           "the JSON response is too large (%zu bytes)",
                           size));
            return;
          }
      }
    }

  state = g_new0 (JsonProcessTask, 1);
  state->is_not_modified = is_not_modified;
  state->encode_cache = encode_cache;
  if (is_not_modified)
    {
      state->stale_json = g_strdup (request->stale_json);
      state->status_error = status_error;
      if (encode_cache)
        state->etag = g_strdup (request->cache_etag);
    }
  else
    {
      state->body = body != NULL ? g_bytes_ref (body) : NULL;
      if (encode_cache)
        state->etag = g_strdup (request->response_etag);
    }

  parse_task = g_task_new (NULL, g_task_get_cancellable (request->task),
                           json_process_body_ready, request);
  g_task_set_task_data (parse_task, state,
                        (GDestroyNotify) json_process_task_free);
  g_task_run_in_thread (parse_task, json_process_body_worker);
}

void
nuvashalauncher_download_json_stream_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DownloadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  GInputStream *stream = soup_session_send_finish (SOUP_SESSION (source),
                                                   result, &error);
  SoupMessageHeaders *headers = NULL;
  goffset length = -1;

  if (stream == NULL)
    {
      end_attempt (request, g_steal_pointer (&error));
      return;
    }

  request->source = stream;

  /* An error status is reported without streaming its body into memory. */
  {
    guint status = soup_message_get_status (request->message);

    if (!SOUP_STATUS_IS_SUCCESSFUL (status) &&
        status != SOUP_STATUS_NOT_MODIFIED)
      {
        end_attempt (request, check_status (request));
        return;
      }
  }

  headers = soup_message_get_response_headers (request->message);
  length = soup_message_headers_get_content_length (headers);
  if (length > (goffset) MAX_JSON_RESPONSE_SIZE)
    {
      end_attempt (request,
                   g_error_new_literal (NUVASHALAUNCHER_DOWNLOAD_ERROR,
                                         NUVASHALAUNCHER_DOWNLOAD_ERROR_INVALID,
                                         "the JSON response is too large"));
      return;
    }
  read_next_chunk (request);
}

/* ---------------------------------------------------------- *
 * The public interface
 * ---------------------------------------------------------- */
