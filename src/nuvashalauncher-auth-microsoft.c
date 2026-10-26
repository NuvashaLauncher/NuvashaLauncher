/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-auth-microsoft-private.h"
#include "nuvashalauncher-task-private.h"

#include <libsoup/soup.h>
#include <string.h>

GQuark
nuvashalauncher_auth_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-auth-error");
}

/* Whether an Azure application is configured for sign-in. */
gboolean
client_id_is_set (void)
{
  return nuvashalauncher_MICROSOFT_CLIENT_ID[0] != '\0';
}

/* Error returned when sign-in is unavailable in this build. */
GError *
no_client_id_error (void)
{
  return g_error_new (
      nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_CLIENT_ID,
      "signing in is not set up in this build: nuvashalauncher_MICROSOFT_CLIENT_ID in "
      "src/nuvashalauncher-auth-microsoft.h is empty, and it has to hold the application "
      "(client) ID of an Azure application registered for the device code "
      "flow of a public client");
}

/* Authentication request setup. */

static SoupSession *
auth_session (void)
{
  /* One session for the life of the process, like the download module's,
     which is why every function of this one has to be called from the
     main context it was first called from. */
  static SoupSession *session = NULL;

  if (session == NULL)
    session = soup_session_new_with_options (
        "timeout", (guint) nuvashalauncher_AUTH_TIMEOUT_SECONDS, "user-agent",
        nuvashalauncher_AUTH_USER_AGENT, NULL);

  return session;
}

/* A sign-in is the task: the request lives as long as the task does, and
   the task is what is answered when it is over. */
AuthRequest *
request_new (nuvashalauncherTaskScope *scope)
{
  AuthRequest *self = g_new0 (AuthRequest, 1);

  /* Borrowed: this request is the task's own data and the scope owns that
     task. The caller keeps the operation alive until its callback. */
  self->task = g_object_ref (nuvashalauncher_task_scope_task (scope));
  self->scope = scope;
  self->now = g_get_real_time () / 1000000;

  /* The cancellation of the task is this module's own: a sign-in that is
     cancelled stops waiting, and nothing is answered twice. */
  g_task_set_check_cancellable (self->task, FALSE);
  g_task_set_task_data (self->task, self, nuvashalauncher_auth_request_free);

  return self;
}

/* The one way out of a sign-in: an error and no account. */
void
fail (AuthRequest *self, GError *error)
{
  g_assert (error != NULL);

  if (self->task == NULL)
    {
      g_error_free (error);
      return;
    }

  /* Deferred: a chain can find out that it has nothing to do before the
     call that started it has returned the handle it must release. */
  nuvashalauncher_task_scope_return_error_deferred (self->scope, error);
  g_clear_object (&self->task);
}

/* Complete the sign-in task and return the resulting account or code. */
void
succeed (AuthRequest   *self,
         gpointer       result,
         GDestroyNotify free_result)
{
  if (self->task == NULL)
    {
      free_result (result);
      return;
    }

  nuvashalauncher_task_scope_return_pointer_deferred (self->scope, result, free_result);
  g_clear_object (&self->task);
}

/* Answers the task with the cancellation and says whether it did, which
   is how a cancelled sign-in stops instead of failing to reach a
   service that was never asked. */
gboolean
answer_if_cancelled (AuthRequest *self)
{
  GTask *task = NULL;

  if (self->task == NULL ||
      !nuvashalauncher_task_scope_return_error_if_cancelled (self->scope))
    return FALSE;

  task = g_steal_pointer (&self->task);
  g_object_unref (task);

  return TRUE;
}

void
device_code_answered (AuthRequest *self);
void token_answered (AuthRequest *self);
void xbl_answered (AuthRequest *self);
void xsts_answered (AuthRequest *self);
void minecraft_answered (AuthRequest *self);
void entitlements_answered (AuthRequest *self);
void profile_answered (AuthRequest *self);
void ask_xbl (AuthRequest *self, const char *access_token);
void ask_xsts (AuthRequest *self);
void ask_minecraft (AuthRequest *self);
void ask_entitlements (AuthRequest *self);
void ask_profile (AuthRequest *self);

/* Sends one request and hands what came back to the step that asked for
 * it. The answer is kept as it arrived, whatever the status was: a
 * service that refuses something says why in the body of the refusal,
 * and that sentence is what a user is told. */
void
nuvashalauncher_auth_send (AuthRequest         *self,
      const char          *url,
      const char          *method,
      const char          *body,
      const char          *content_type,
      const char         **headers,
      Step                 step)
{
  SoupMessageHeaders *request_headers = NULL;

  g_assert (self->message == NULL);

  self->step = step;
  self->message = soup_message_new (method, url);

  if (body != NULL)
    soup_message_set_request_body_from_bytes (
        self->message,
        content_type != NULL ? content_type : "application/json",
        g_bytes_new (body, strlen (body)));

  request_headers = soup_message_get_request_headers (self->message);

  if (content_type != NULL)
    soup_message_headers_replace (request_headers, "Content-Type",
                                  content_type);

  for (guint i = 0; headers != NULL && headers[i] != NULL; i += 2)
    soup_message_headers_replace (request_headers, headers[i], headers[i + 1]);

  soup_session_send_async (
       auth_session (), self->message, G_PRIORITY_DEFAULT,
       g_task_get_cancellable (self->task), (GAsyncReadyCallback) sent, self);
}

void
sent (SoupSession         *session G_GNUC_UNUSED,
      GAsyncResult        *result,
      gpointer             user_data)
{
  /* The request belongs to the task: the last thing this callback does is
     answer the task, and nothing here touches the request after that. */
  AuthRequest *self = user_data;
  g_autoptr (GError) transport_error = NULL;

  self->status = soup_message_get_status (self->message);
  self->response_stream = soup_session_send_finish (auth_session (), result,
                                                    &transport_error);

  /* A request that never came back is not a service that said no. */
  if (transport_error != NULL)
    {
      if (g_error_matches (transport_error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          answer_if_cancelled (self))
        return;

      fail (self,
            g_error_new (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NETWORK,
                         "the sign-in service could not be reached: %s",
                         transport_error->message));
      return;
    }

  self->response_bytes = g_byte_array_new ();
  if (soup_message_headers_get_content_length (
        soup_message_get_response_headers (self->message)) >
      (goffset) MAX_AUTH_RESPONSE_SIZE)
    {
      fail (self, g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NETWORK,
                                       "the sign-in response is too large"));
      return;
    }
  g_input_stream_read_bytes_async (self->response_stream, 64 * 1024,
                                   G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (self->task),
                                   auth_read_ready, self);
}

void
auth_read_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AuthRequest *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) chunk =
    g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);
  gsize size = 0;

  if (chunk == NULL)
    {
      fail (self, g_steal_pointer (&error));
      return;
    }

  size = g_bytes_get_size (chunk);
  if (self->response_bytes->len + size > MAX_AUTH_RESPONSE_SIZE)
    {
      fail (self, g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NETWORK,
                                       "the sign-in response is too large"));
      return;
    }

  if (size != 0)
    g_byte_array_append (self->response_bytes, g_bytes_get_data (chunk, NULL),
                         size);
  if (size != 0)
    {
      g_input_stream_read_bytes_async (self->response_stream, 64 * 1024,
                                       G_PRIORITY_DEFAULT,
                                       g_task_get_cancellable (self->task),
                                       auth_read_ready, self);
      return;
    }

  g_free (self->body);
  self->body = g_strndup ((const char *) self->response_bytes->data,
                          self->response_bytes->len);
  g_clear_pointer (&self->response_bytes, g_byte_array_unref);
  g_clear_object (&self->response_stream);
  g_clear_object (&self->message);

  /* The step that asked for this request is the only thing a sign-in
     keeps between two of them. */
  switch (self->step)
    {
    case STEP_DEVICE_CODE:
      device_code_answered (self);
      break;

    case STEP_TOKEN:
      token_answered (self);
      break;

    case STEP_XBL:
      xbl_answered (self);
      break;

    case STEP_XSTS:
      xsts_answered (self);
      break;

    case STEP_MINECRAFT:
      minecraft_answered (self);
      break;

    case STEP_ENTITLEMENTS:
      entitlements_answered (self);
      break;

    case STEP_PROFILE:
    default:
      profile_answered (self);
      break;
    }
}

/* What a service said when it refused something: the sentence it gave,
   and never the whole answer, which on a success is the token. */
GError *
refusal (const char  *body,
         gint          status,
         nuvashalauncherAuthError  code,
         const char   *what)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *said = NULL;
  JsonNode *root = NULL;

  if (body != NULL && *body != '\0')
    {
      parser = json_parser_new ();

      if (json_parser_load_from_data (parser, body, -1, NULL))
        {
          root = json_parser_get_root (parser);

          if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
            {
              JsonObject *object = json_node_get_object (root);
              const char *members[] = { "error_description", "errorMessage",
                                        "Message", NULL };

              for (guint i = 0; members[i] != NULL; i++)
                {
                  JsonNode *node = json_object_get_member (object, members[i]);

                  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
                      json_node_get_value_type (node) == G_TYPE_STRING)
                    {
                      said = g_strdup (json_node_get_string (node));
                      break;
                    }
                }
            }
        }
    }

  return g_error_new (nuvashalauncher_AUTH_ERROR, code, "%s (HTTP %d)%s%s", what, status,
                      said != NULL && *said != '\0' ? ": " : "",
                      said != NULL ? said : "");
}
