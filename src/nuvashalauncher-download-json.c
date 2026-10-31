/* Download JSON response handling. */

#include "nuvashalauncher-download-private.h"

void
nuvashalauncher_download_json_process_body (DownloadRequest *request, GBytes *body)
{
  g_autoptr (GError) error = NULL;
  GError *status_error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) parse_error = NULL;
  g_autofree char *response = NULL;
  gsize size = 0;
  const char *data = NULL;

  status_error = check_status (request);

  if (request->stale_json != NULL &&
      soup_message_get_status (request->message) == SOUP_STATUS_NOT_MODIFIED)
    {
      g_autoptr (JsonParser) stale = json_parser_new ();

      if (json_parser_load_from_data (stale, request->stale_json, -1, NULL) &&
          json_parser_get_root (stale) != NULL)
        {
          g_clear_error (&status_error);
          nuvashalauncher_download_request_succeed_json (request, g_steal_pointer (&stale));
          return;
        }
    }

  if (status_error != NULL)
    {
      end_attempt (request, status_error);
      return;
    }

  parser = json_parser_new ();
  data = g_bytes_get_data (body, &size);

  {
    const char *etag = soup_message_headers_get_one (
      soup_message_get_response_headers (request->message), "ETag");

    g_clear_pointer (&request->response_etag, g_free);
    request->response_etag = g_strdup (etag);
  }

  if (size > MAX_JSON_RESPONSE_SIZE)
    {
      nuvashalauncher_download_request_fail (request,
                    g_error_new (nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_INVALID,
                                 "the JSON response is too large (%zu bytes)",
                                 size));
      return;
    }

  if (!json_parser_load_from_data (parser, data != NULL ? data : "",
                                   (gssize) size, &parse_error))
    {
      /* A body that is not JSON would come back unchanged on a retry,
         so the parse error is reported as it is. */
      nuvashalauncher_download_request_fail (request, g_steal_pointer (&parse_error));
      return;
    }

  if (json_parser_get_root (parser) == NULL)
    {
      /* json-glib accepts an empty document without complaint; there
         is nothing to hand to a caller though. */
      nuvashalauncher_download_request_fail (request,
                    g_error_new_literal (JSON_PARSER_ERROR,
                                         JSON_PARSER_ERROR_PARSE,
                                         "the response body holds no JSON"));
      return;
    }

  if (request->api_cache && request->cache_path != NULL)
    {
      data = g_bytes_get_data (body, &size);
      response = g_strndup (data, size);
      request->pending_parser = g_steal_pointer (&parser);
      nuvashalauncher_download_cache_write (request, response);
      return;
    }

  nuvashalauncher_download_request_succeed_json (request, g_steal_pointer (&parser));
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
  headers = soup_message_get_response_headers (request->message);
  length = soup_message_headers_get_content_length (headers);
  if (length > (goffset) MAX_JSON_RESPONSE_SIZE)
    {
      end_attempt (request,
                   g_error_new_literal (nuvashalauncher_DOWNLOAD_ERROR,
                                         nuvashalauncher_DOWNLOAD_ERROR_INVALID,
                                         "the JSON response is too large"));
      return;
    }
  read_next_chunk (request);
}

/* ---------------------------------------------------------- *
 * The public interface
 * ---------------------------------------------------------- */
