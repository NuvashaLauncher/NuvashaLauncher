/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-task-private.h"

#include <libsoup/soup.h>
#include <string.h>

#define MAX_AUTH_RESPONSE_SIZE (4ULL * 1024ULL * 1024ULL)

typedef enum {
  STEP_DEVICE_CODE,
  STEP_TOKEN,
  STEP_XBL,
  STEP_XSTS,
  STEP_MINECRAFT,
  STEP_ENTITLEMENTS,
  STEP_PROFILE
} Step;

typedef struct _AuthRequest {
  GTask *task;
  SoupMessage *message;
  guint poll_source;
  NuvashaLauncherTaskScope *scope;
  Step step;
  gint status;
  char *body;
  GInputStream *response_stream;
  GByteArray *response_bytes;
  char *device_code;
  char *refresh_token;
  guint interval;
  gint64 deadline;
  gint64 now;
  char *microsoft_refresh_token;
  char *xbl_token;
  char *xbl_user_hash;
  char *xsts_token;
  char *mc_access_token;
  gint64 mc_expires_at;
} AuthRequest;

static NuvashaLauncherAuthTransportFunc test_transport = NULL;
static gpointer test_transport_data = NULL;
static const char *test_client_id = NULL;

static void auth_send (AuthRequest *self, const char *url, const char *method,
                       const char *body, const char *content_type,
                       const char **headers, Step step);
static void sent (SoupSession *session, GAsyncResult *result, gpointer user_data);
static void auth_read_ready (GObject *source, GAsyncResult *result, gpointer user_data);
static void dispatch_step (AuthRequest *self);
static void device_code_answered (AuthRequest *self);
static void ask_for_token (AuthRequest *self);
static void token_answered (AuthRequest *self);
static void ask_xbl (AuthRequest *self, const char *access_token);
static void xbl_answered (AuthRequest *self);
static void ask_xsts (AuthRequest *self);
static void xsts_answered (AuthRequest *self);
static void ask_minecraft (AuthRequest *self);
static void minecraft_answered (AuthRequest *self);
static void ask_entitlements (AuthRequest *self);
static void entitlements_answered (AuthRequest *self);
static void ask_profile (AuthRequest *self);
static void profile_answered (AuthRequest *self);

void
nuvashalauncher_auth_set_transport_for_tests (NuvashaLauncherAuthTransportFunc transport,
                                              gpointer                         user_data)
{
  test_transport = transport;
  test_transport_data = user_data;
}

void
nuvashalauncher_auth_set_client_id_for_tests (const char *client_id)
{
  test_client_id = client_id;
}

GQuark
nuvashalauncher_auth_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-auth-error");
}

static const char *
active_client_id (void)
{
  return test_client_id != NULL ? test_client_id : nuvashalauncher_MICROSOFT_CLIENT_ID;
}

/* Whether an Azure application is configured for sign-in. */
static gboolean
client_id_is_set (void)
{
  return active_client_id ()[0] != '\0';
}

/* Error returned when sign-in is unavailable in this build. */
static GError *
no_client_id_error (void)
{
  return g_error_new (
      nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_CLIENT_ID,
      "signing in is not set up in this build: nuvashalauncher_MICROSOFT_CLIENT_ID in "
      "src/nuvashalauncher-auth-microsoft.h is empty, and it has to hold the application "
      "(client) ID of an Azure application registered for the device code "
      "flow of a public client");
}

/* Authentication request setup & lifecycle. */

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

static void
auth_request_free (gpointer data)
{
  AuthRequest *self = data;

  /* The scope owns the task this request is the data of, so reaching here
     is the scope going away: it has already released the sources the poll
     was waiting on. */

  g_clear_object (&self->task);
  g_clear_object (&self->message);
  g_clear_object (&self->response_stream);
  g_clear_pointer (&self->response_bytes, g_byte_array_unref);
  g_free (self->body);
  g_free (self->device_code);
  g_free (self->refresh_token);
  g_free (self->microsoft_refresh_token);
  g_free (self->xbl_token);
  g_free (self->xbl_user_hash);
  g_free (self->xsts_token);
  g_free (self->mc_access_token);
  g_free (self);
}

/* A sign-in is the task: the request lives as long as the task does, and
   the task is what is answered when it is over. */
static AuthRequest *
request_new (NuvashaLauncherTaskScope *scope)
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
  g_task_set_task_data (self->task, self, auth_request_free);

  return self;
}

/* The one way out of a sign-in: an error and no account. */
static void
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
static void
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
static gboolean
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

/* What a service said when it refused something: the sentence it gave,
   and never the whole answer, which on a success is the token. */
static GError *
refusal (const char               *body,
         gint                      status,
         NuvashaLauncherAuthError  code,
         const char               *what)
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

/* HTTP transport & response dispatch. */

static void
dispatch_step (AuthRequest *self)
{
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

/* Sends one request and hands what came back to the step that asked for
 * it. The answer is kept as it arrived, whatever the status was: a
 * service that refuses something says why in the body of the refusal,
 * and that sentence is what a user is told. */
static void
auth_send (AuthRequest  *self,
           const char   *url,
           const char   *method,
           const char   *body,
           const char   *content_type,
           const char  **headers,
           Step          step)
{
  SoupMessageHeaders *request_headers = NULL;

  self->step = step;

  if (test_transport != NULL)
    {
      g_autoptr (GError) transport_error = NULL;
      gint status = 0;
      char *response_body = NULL;

      if (!test_transport (url, method, body, headers, &status, &response_body,
                           test_transport_data, &transport_error))
        {
          if (transport_error == NULL)
            transport_error = g_error_new_literal (
                nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NETWORK,
                "the sign-in service could not be reached");
          fail (self, g_steal_pointer (&transport_error));
          return;
        }

      self->status = status;
      g_free (self->body);
      self->body = response_body != NULL ? response_body : g_strdup ("");
      dispatch_step (self);
      return;
    }

  g_assert (self->message == NULL);

  self->message = soup_message_new (method, url);
  if (self->message == NULL)
    {
      fail (self, g_error_new (nuvashalauncher_AUTH_ERROR,
                               nuvashalauncher_AUTH_ERROR_SERVICE,
                               "Invalid request URL \"%s\"", url));
      return;
    }

  if (body != NULL)
    {
      g_autoptr (GBytes) request_body = g_bytes_new (body, strlen (body));

      soup_message_set_request_body_from_bytes (
          self->message,
          content_type != NULL ? content_type : "application/json",
          request_body);
    }

  request_headers = soup_message_get_request_headers (self->message);

  for (guint i = 0; headers != NULL && headers[i] != NULL; i += 2)
    soup_message_headers_replace (request_headers, headers[i], headers[i + 1]);

  soup_session_send_async (
       auth_session (), self->message, G_PRIORITY_DEFAULT,
       g_task_get_cancellable (self->task), (GAsyncReadyCallback) sent, self);
}

static void
sent (SoupSession  *session G_GNUC_UNUSED,
      GAsyncResult *result,
      gpointer      user_data)
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
      fail (self, g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_PARSE,
                                       "the sign-in response is too large"));
      return;
    }
  g_input_stream_read_bytes_async (self->response_stream, 64 * 1024,
                                   G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (self->task),
                                   auth_read_ready, self);
}

static void
auth_read_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AuthRequest *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) chunk =
    g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);
  gsize size = 0;

  if (chunk == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          answer_if_cancelled (self))
        return;

      fail (self, g_steal_pointer (&error));
      return;
    }

  size = g_bytes_get_size (chunk);
  if (self->response_bytes->len + size > MAX_AUTH_RESPONSE_SIZE)
    {
      fail (self, g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_PARSE,
                                       "the sign-in response is too large"));
      return;
    }

  if (size != 0)
    {
      g_byte_array_append (self->response_bytes, g_bytes_get_data (chunk, NULL),
                           size);
      g_input_stream_read_bytes_async (self->response_stream, 64 * 1024,
                                       G_PRIORITY_DEFAULT,
                                       g_task_get_cancellable (self->task),
                                       auth_read_ready, self);
      return;
    }

  g_free (self->body);
  self->body = self->response_bytes->data != NULL
                 ? g_strndup ((const char *) self->response_bytes->data,
                              self->response_bytes->len)
                 : g_strdup ("");
  g_clear_pointer (&self->response_bytes, g_byte_array_unref);
  g_clear_object (&self->response_stream);
  g_clear_object (&self->message);

  dispatch_step (self);
}

/* ---------------------------------------------------------- *
 * Step 1: Device code grant
 * ---------------------------------------------------------- */

static void
device_code_answered (AuthRequest *self)
{
  g_autoptr (NuvashaLauncherDeviceCode) code = NULL;
  g_autoptr (GError) error = NULL;

  if (!SOUP_STATUS_IS_SUCCESSFUL (self->status))
    {
      fail (self,
            refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_DEVICE_CODE,
                     _("Microsoft would not give a code to sign in with")));
      return;
    }

  code = nuvashalauncher_auth_device_code_from_json (self->body, -1, &error);

  if (code == NULL)
    fail (self, g_steal_pointer (&error));
  else
    succeed (self, g_steal_pointer (&code),
             (GDestroyNotify) nuvashalauncher_device_code_free);
}

NuvashaLauncherTaskScope *
nuvashalauncher_auth_device_code_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autofree char *form = NULL;
  AuthRequest *self = NULL;

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  form = soup_form_encode ("client_id", active_client_id (), "scope",
                           nuvashalauncher_MICROSOFT_SCOPE, NULL);

  auth_send (self, nuvashalauncher_MICROSOFT_DEVICE_CODE_URL, SOUP_METHOD_POST, form,
             "application/x-www-form-urlencoded", NULL, STEP_DEVICE_CODE);

  return g_steal_pointer (&scope);
}

NuvashaLauncherDeviceCode *
nuvashalauncher_auth_device_code_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (NuvashaLauncherDeviceCode *) g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Step 2: Token polling & refresh
 * ---------------------------------------------------------- */

static char *
token_form (AuthRequest *self)
{
  const char *client_id = active_client_id ();

  if (self->refresh_token != NULL)
    return soup_form_encode ("client_id", client_id,
                             "grant_type", "refresh_token", "refresh_token",
                             self->refresh_token, "scope",
                             nuvashalauncher_MICROSOFT_SCOPE, NULL);

  return soup_form_encode ("client_id", client_id, "grant_type",
                           "urn:ietf:params:oauth:grant-type:device_code",
                           "device_code", self->device_code, "scope",
                           nuvashalauncher_MICROSOFT_SCOPE, NULL);
}

static void
ask_for_token (AuthRequest *self)
{
  g_autofree char *form = token_form (self);

  auth_send (self, nuvashalauncher_MICROSOFT_TOKEN_URL, SOUP_METHOD_POST, form,
             "application/x-www-form-urlencoded", NULL, STEP_TOKEN);
}

/* The next poll, once the interval the service asked for has passed. */
static gboolean
poll_again (gpointer user_data)
{
  AuthRequest *self = user_data;
  guint source_id = self->poll_source;

  self->poll_source = 0;
  nuvashalauncher_task_scope_forget_source (self->scope, source_id);

  if (self->task == NULL || answer_if_cancelled (self))
    return G_SOURCE_REMOVE;

  ask_for_token (self);

  return G_SOURCE_REMOVE;
}

static void
token_answered (AuthRequest *self)
{
  g_autoptr (NuvashaLauncherOAuthToken) token = NULL;
  g_autoptr (GError) error = NULL;
  gboolean slow_down = FALSE;

  if (self->status == SOUP_STATUS_OK)
    {
      token = nuvashalauncher_auth_oauth_token_from_json (self->body, -1, g_get_real_time () / 1000000, &error);

      if (token == NULL)
        {
          fail (self, g_steal_pointer (&error));
          return;
        }

      /* The refresh token is what the account keeps, so that the sign-in
         outlives the token it starts with. */
      if (token->refresh_token != NULL)
        {
          g_free (self->microsoft_refresh_token);
          self->microsoft_refresh_token = g_strdup (token->refresh_token);
        }

      ask_xbl (self, token->access_token);
      return;
    }

  /* Every poll answers that the user has not signed in yet until they
     have, and that is the wait rather than a failure. */
  if (nuvashalauncher_auth_polling_error (self->body, -1, &slow_down, &error))
    {
      if (error == NULL)
        error = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_PARSE,
                                     "the service refused the sign-in without "
                                     "saying why");

      fail (self, g_steal_pointer (&error));
      return;
    }

  /* A refresh request has no device code and must not poll. */
  if (self->device_code == NULL)
    {
      fail (self,
            refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_REAUTH,
                     "the sign-in expired"));
      return;
    }

  /* Respect the service's request to slow down polling. */
  if (slow_down)
    self->interval += 5;

  /* The code is good for a quarter of an hour and no longer, so a user
     who walked away is told rather than left waiting. */
  if (self->deadline > 0 &&
      g_get_real_time () / 1000000 >= self->deadline)
    {
      fail (self,
            g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_EXPIRED,
                                 "the code ran out before the sign-in was "
                                 "finished: ask for a new one"));
      return;
    }

  self->poll_source = g_timeout_add_seconds (self->interval, poll_again, self);
  g_source_set_name_by_id (self->poll_source, "nuvashalauncher-auth-token-poll");
  nuvashalauncher_task_scope_track_source (self->scope, self->poll_source);
}

/* ---------------------------------------------------------- *
 * Step 3: Xbox Live authentication
 * ---------------------------------------------------------- */

static void
ask_xbl (AuthRequest *self, const char *access_token)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *body = NULL;
  g_autofree char *rps_ticket = NULL;
  const char *headers[] = { "x-xbl-contract-version",
                            nuvashalauncher_XBOX_CONTRACT_VERSION, "Accept",
                            "application/json", NULL };

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "Properties");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "AuthMethod");
  json_builder_add_string_value (builder, "RPS");
  json_builder_set_member_name (builder, "SiteName");
  json_builder_add_string_value (builder, "user.auth.xboxlive.com");
  rps_ticket = g_strdup_printf ("d=%s", access_token);

  json_builder_set_member_name (builder, "RpsTicket");
  json_builder_add_string_value (builder, rps_ticket);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "RelyingParty");
  json_builder_add_string_value (builder, "http://auth.xboxlive.com");
  json_builder_set_member_name (builder, "TokenType");
  json_builder_add_string_value (builder, "JWT");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  body = json_generator_to_data (generator, NULL);
  auth_send (self, nuvashalauncher_XBOX_USER_URL, SOUP_METHOD_POST, body,
             "application/json", headers, STEP_XBL);
}

static void
xbl_answered (AuthRequest *self)
{
  g_autoptr (NuvashaLauncherXboxToken) token = NULL;
  g_autoptr (GError) error = NULL;

  if (!SOUP_STATUS_IS_SUCCESSFUL (self->status))
    {
      fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_XBOX,
                           "Xbox Live refused the sign-in"));
      return;
    }

  token = nuvashalauncher_auth_xbox_token_from_json (self->body, -1, &error);

  if (token == NULL)
    {
      fail (self, g_steal_pointer (&error));
      return;
    }

  g_free (self->xbl_token);
  self->xbl_token = g_steal_pointer (&token->token);
  g_free (self->xbl_user_hash);
  self->xbl_user_hash = g_steal_pointer (&token->user_hash);

  ask_xsts (self);
}

/* ---------------------------------------------------------- *
 * Step 4: XSTS authorization
 * ---------------------------------------------------------- */

static void
ask_xsts (AuthRequest *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *body = NULL;
  const char *headers[] = { "x-xbl-contract-version",
                            nuvashalauncher_XBOX_CONTRACT_VERSION, "Accept",
                            "application/json", NULL };

  /* The relying party is what says which services the token is for, and
     the ones being asked for are the Minecraft services. */
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "Properties");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "SandboxId");
  json_builder_add_string_value (builder, "RETAIL");
  json_builder_set_member_name (builder, "UserTokens");
  json_builder_begin_array (builder);
  json_builder_add_string_value (builder, self->xbl_token);
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "RelyingParty");
  json_builder_add_string_value (builder, nuvashalauncher_MINECRAFT_RELYING_PARTY);
  json_builder_set_member_name (builder, "TokenType");
  json_builder_add_string_value (builder, "JWT");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  body = json_generator_to_data (generator, NULL);

  auth_send (self, nuvashalauncher_XBOX_XSTS_URL, SOUP_METHOD_POST, body, "application/json",
             headers, STEP_XSTS);
}

static void
xsts_answered (AuthRequest *self)
{
  g_autoptr (NuvashaLauncherXboxToken) token = NULL;
  g_autoptr (GError) error = NULL;

  token = nuvashalauncher_auth_xbox_token_from_json (self->body, -1, &error);

  if (token == NULL)
    {
      if (error != NULL)
        fail (self, g_steal_pointer (&error));
      else if (!SOUP_STATUS_IS_SUCCESSFUL (self->status))
        fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_XBOX,
                             "XSTS refused the token request"));
      else
        fail (self, g_error_new_literal (nuvashalauncher_AUTH_ERROR,
                                         nuvashalauncher_AUTH_ERROR_PARSE,
                                         "XSTS answered with no token"));
      return;
    }

  if (!SOUP_STATUS_IS_SUCCESSFUL (self->status))
    {
      fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_XBOX,
                           "Xbox services refused the sign-in"));
      return;
    }

  g_free (self->xsts_token);
  self->xsts_token = g_steal_pointer (&token->token);

  /* The services are told who the player is by the hash the user token
     came with, and without it there is nobody to sign in as. */
  if (self->xbl_user_hash == NULL)
    {
      fail (self, g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_PARSE,
                                       "Xbox services answered with a token "
                                       "and no user"));
      return;
    }

  ask_minecraft (self);
}

/* ---------------------------------------------------------- *
 * Step 5: Minecraft services login
 * ---------------------------------------------------------- */

static void
ask_minecraft (AuthRequest *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *xtoken = NULL;
  g_autofree char *body = NULL;
  const char *headers[] = { "Accept", "application/json", NULL };

  /* Current Minecraft services use the launcher login contract. The request
   * sends the XBL3 token and explicitly identifies the PC launcher;
   * the older login_with_xbox identityToken endpoint can return a token that
   * is not accepted by the entitlement/profile services. */
  xtoken = g_strdup_printf ("XBL3.0 x=%s;%s", self->xbl_user_hash,
                            self->xsts_token);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "xtoken");
  json_builder_add_string_value (builder, xtoken);
  json_builder_set_member_name (builder, "platform");
  json_builder_add_string_value (builder, "PC_LAUNCHER");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  body = json_generator_to_data (generator, NULL);

  auth_send (self, nuvashalauncher_MINECRAFT_SERVICES_URL "/launcher/login",
             SOUP_METHOD_POST, body, "application/json", headers, STEP_MINECRAFT);
}

static void
minecraft_answered (AuthRequest *self)
{
  g_autoptr (NuvashaLauncherOAuthToken) token = NULL;
  g_autoptr (GError) error = NULL;

  if (!SOUP_STATUS_IS_SUCCESSFUL (self->status))
    {
      /* "Invalid app registration" is what the services answer an Azure
         application that is not registered for Minecraft with, and it
         comes through in the sentence they give. */
      fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_XBOX,
                           "the Minecraft services refused the sign-in"));
      return;
    }

  token = nuvashalauncher_auth_minecraft_token_from_json (self->body, -1,
                                                          g_get_real_time () / 1000000,
                                                          &error);

  if (token == NULL)
    {
      fail (self, g_steal_pointer (&error));
      return;
    }

  g_free (self->mc_access_token);
  self->mc_access_token = g_steal_pointer (&token->access_token);
  self->mc_expires_at = token->expires_at;

  ask_entitlements (self);
}

/* ---------------------------------------------------------- *
 * Step 6: Entitlements check
 * ---------------------------------------------------------- */

static void
ask_entitlements (AuthRequest *self)
{
  g_autofree char *authorization = NULL;
  g_autofree char *url = NULL;
  g_autofree char *request_id = g_uuid_string_random ();
  const char *headers[] = { "Authorization", NULL, "Accept",
                            "application/json", NULL };

  authorization = g_strconcat ("Bearer ", self->mc_access_token, NULL);
  headers[1] = authorization;

  /* Current Minecraft services use the license endpoint with a request id.
     The older mcstore endpoint can return a legacy-shaped
     response or reject otherwise valid accounts. */
  url = g_strdup_printf (nuvashalauncher_MINECRAFT_SERVICES_URL
                         "/entitlements/license?requestId=%s", request_id);
  auth_send (self, url, SOUP_METHOD_GET, NULL, NULL, headers, STEP_ENTITLEMENTS);
}

static void
entitlements_answered (AuthRequest *self)
{
  g_autoptr (GError) error = NULL;

  if (!SOUP_STATUS_IS_SUCCESSFUL (self->status))
    {
      fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_XBOX,
                           "the Minecraft services did not answer with what "
                           "this account owns"));
      return;
    }

  if (!nuvashalauncher_auth_owns_minecraft_from_json (self->body, -1, &error))
    fail (self, g_steal_pointer (&error));
  else
    ask_profile (self);
}

/* ---------------------------------------------------------- *
 * Step 7: Minecraft profile
 * ---------------------------------------------------------- */

static void
ask_profile (AuthRequest *self)
{
  g_autofree char *authorization = NULL;
  const char *headers[] = { "Authorization", NULL, "Accept",
                            "application/json", NULL };

  authorization = g_strconcat ("Bearer ", self->mc_access_token, NULL);
  headers[1] = authorization;

  auth_send (self, nuvashalauncher_MINECRAFT_SERVICES_URL "/minecraft/profile",
             SOUP_METHOD_GET, NULL, NULL, headers, STEP_PROFILE);
}

static void
profile_answered (AuthRequest *self)
{
  g_autoptr (NuvashaLauncherMinecraftProfile) profile = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = NULL;
  NuvashaLauncherAccount *account = NULL;

  profile = nuvashalauncher_auth_profile_from_json (self->body, -1, &error);

  if (profile == NULL)
    {
      /* The services answer 404 for an account that owns the game and has
         never played it, which the parser says in its own words rather
         than as a service that could not be reached. */
      if (error != NULL)
        fail (self, g_steal_pointer (&error));
      else if (self->status == SOUP_STATUS_NOT_FOUND)
        fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_NO_PROFILE,
                             "the Minecraft services have no profile for this "
                             "account"));
      else
        fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_SERVICE,
                             "the Minecraft profile service refused the request"));
      return;
    }

  uuid = nuvashalauncher_auth_profile_uuid (profile->id);

  if (uuid == NULL)
    {
      fail (self,
            g_error_new (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_PARSE,
                         "the Minecraft services answered with a profile id "
                         "that is not a uuid"));
      return;
    }

  /* What the launcher is handed: who the player is, and the token it is
     run with. Nothing else of the chain is kept. */
  account = g_new0 (NuvashaLauncherAccount, 1);
  account->type = nuvashalauncher_ACCOUNT_MICROSOFT;
  account->id = g_strdup (profile->id);
  account->name = g_strdup (profile->name);
  account->uuid = g_steal_pointer (&uuid);
  account->access_token = g_strdup (self->mc_access_token);
  account->expires_at = self->mc_expires_at;
  account->refresh_token = g_strdup (self->microsoft_refresh_token);
  account->avatar_url = g_strdup (profile->avatar_url);
  account->skin_model = g_strdup (profile->skin_model);
  account->cape_id = g_strdup (profile->cape_id);

  succeed (self, account, (GDestroyNotify) nuvashalauncher_account_free);
}

/* ---------------------------------------------------------- *
 * Public sign-in and refresh entry points
 * ---------------------------------------------------------- */

NuvashaLauncherTaskScope *
nuvashalauncher_auth_sign_in_async (const NuvashaLauncherDeviceCode *code,
                                    GAsyncReadyCallback              callback,
                                    gpointer                         user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  AuthRequest *self = NULL;

  nuvashalauncher_task_scope_set_source_tag (scope, "nuvashalauncher_auth_sign_in_async");

  g_return_val_if_fail (code != NULL, NULL);
  g_return_val_if_fail (code->device_code != NULL, NULL);

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  self->device_code = g_strdup (code->device_code);
  self->interval = code->interval > 0 ? code->interval : 5;
  self->deadline = self->now + code->expires_in;

  ask_for_token (self);

  return g_steal_pointer (&scope);
}

NuvashaLauncherAccount *
nuvashalauncher_auth_sign_in_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result,
                                                               "nuvashalauncher_auth_sign_in_async"), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (NuvashaLauncherAccount *) g_task_propagate_pointer (G_TASK (result), error);
}

NuvashaLauncherTaskScope *
nuvashalauncher_auth_refresh_async (const NuvashaLauncherAccount *account,
                                    GAsyncReadyCallback           callback,
                                    gpointer                      user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  AuthRequest *self = NULL;

  nuvashalauncher_task_scope_set_source_tag (scope, "nuvashalauncher_auth_refresh_async");

  g_return_val_if_fail (account != NULL, NULL);

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  /* An account with no refresh token has nothing to be refreshed with,
     and the only thing to be done about it is for the user to sign in
     again: what the caller is told. */
  if (account->refresh_token == NULL || *account->refresh_token == '\0')
    {
      fail (self,
            g_error_new (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_REAUTH,
                         "\"%s\" has to sign in again before it can play",
                         account->name != NULL ? account->name
                                               : "this account"));
      return g_steal_pointer (&scope);
    }

  self->refresh_token = g_strdup (account->refresh_token);
  /* Microsoft does not always return a new refresh token on a refresh;
     keep the one the account already holds when it does not. */
  self->microsoft_refresh_token = g_strdup (account->refresh_token);

  ask_for_token (self);

  return g_steal_pointer (&scope);
}

NuvashaLauncherAccount *
nuvashalauncher_auth_refresh_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result,
                                                               "nuvashalauncher_auth_refresh_async"), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (NuvashaLauncherAccount *) g_task_propagate_pointer (G_TASK (result), error);
}
