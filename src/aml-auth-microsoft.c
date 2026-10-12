/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-auth-microsoft.h"
#include "aml-task-private.h"

#include <libsoup/soup.h>
#include <string.h>

GQuark
aml_auth_error_quark (void)
{
  return g_quark_from_static_string ("aml-auth-error");
}

/* Whether there is an Azure application to sign in with at all. The one
 * constant in this module is what decides it. */
static gboolean
client_id_is_set (void)
{
  return AML_MICROSOFT_CLIENT_ID[0] != '\0';
}

/* What a build without one says instead of asking a user for a sign-in it
 * cannot finish. It names the constant, because a build is not something
 * a user can go and change, and it says what it has to hold. */
static GError *
no_client_id_error (void)
{
  return g_error_new (
      AML_AUTH_ERROR, AML_AUTH_ERROR_NO_CLIENT_ID,
      "signing in is not set up in this build: AML_MICROSOFT_CLIENT_ID in "
      "src/aml-auth-microsoft.h is empty, and it has to hold the application "
      "(client) ID of an Azure application registered for the device code "
      "flow of a public client");
}

/* ---------------------------------------------------------- *
 * Reading an answer
 * ---------------------------------------------------------- */

/* Parses @json and hands out the object at its root. Nothing else of a
 * service is looked at: what it answered is all there is to read. */
static gboolean
open_object (const char          *json,
             gssize               length,
             JsonParser         **parser,
             GError             **error)
{
  g_autoptr (GError) local_error = NULL;
  JsonNode *root = NULL;

  *parser = json_parser_new ();

  if (!json_parser_load_from_data (*parser, json, length, &local_error))
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_PARSE,
                   "the service answered something that is not a document: "
                   "%s", local_error->message);
      g_clear_object (parser);
      return FALSE;
    }

  root = json_parser_get_root (*parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_PARSE,
                   "the service answered something that is not a document");
      g_clear_object (parser);
      return FALSE;
    }

  return TRUE;
}

/* The string member @member of @object, or NULL when it is not one. */
static char *
string_of (JsonObject *object, const char *member)
{
  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;

  return g_strdup (json_node_get_string (node));
}

static gboolean
int_of (JsonObject *object, const char *member, gint64 *value)
{
  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_INT64)
    return FALSE;

  *value = json_node_get_int (node);

  return TRUE;
}

/* ---------------------------------------------------------- *
 * The device authorization answer
 * ---------------------------------------------------------- */

void
aml_device_code_free (AmlDeviceCode *code)
{
  if (code == NULL)
    return;

  g_free (code->user_code);
  g_free (code->verification_uri);
  g_free (code->message);
  g_free (code->device_code);
  g_free (code);
}

AmlDeviceCode *
aml_auth_device_code_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *user_code = NULL;
  g_autofree char *verification_uri = NULL;
  g_autofree char *message = NULL;
  g_autofree char *device_code = NULL;
  AmlDeviceCode *code = NULL;
  JsonObject *object = NULL;
  gint64 expires_in = 0;
  gint64 interval = 5;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));

  user_code = string_of (object, "user_code");
  verification_uri = string_of (object, "verification_uri");
  device_code = string_of (object, "device_code");
  message = string_of (object, "message");
  int_of (object, "expires_in", &expires_in);

  /* The interval is how often the token endpoint may be asked, and a
     service that says nothing about it means once every five seconds. */
  if (!int_of (object, "interval", &interval) || interval < 1)
    interval = 5;

  /* A code without a place to type it, or without a code to type, is
     nothing a user could sign in with. The device code itself is what the
     chain polls with and the caller does not have to see it. */
  if (user_code == NULL || *user_code == '\0' ||
      verification_uri == NULL || *verification_uri == '\0' ||
      device_code == NULL || *device_code == '\0')
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_DEVICE_CODE,
                   "Microsoft did not answer with a code to sign in with");
      return NULL;
    }

  code = g_new0 (AmlDeviceCode, 1);
  code->user_code = g_steal_pointer (&user_code);
  code->verification_uri = g_steal_pointer (&verification_uri);
  code->message = g_steal_pointer (&message);
  code->expires_in = expires_in > 0 ? (guint) expires_in : 900;
  code->interval = (guint) interval;
  code->device_code = g_steal_pointer (&device_code);

  return code;
}

/* ---------------------------------------------------------- *
 * The token answer
 * ---------------------------------------------------------- */

void
aml_oauth_token_free (AmlOAuthToken *token)
{
  if (token == NULL)
    return;

  g_free (token->access_token);
  g_free (token->refresh_token);
  g_free (token);
}

AmlOAuthToken *
aml_auth_oauth_token_from_json (const char *json, gssize length, gint64 now,
                                GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  AmlOAuthToken *token = NULL;
  JsonObject *object = NULL;
  gint64 expires_in = 0;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  int_of (object, "expires_in", &expires_in);

  token = g_new0 (AmlOAuthToken, 1);
  token->access_token = string_of (object, "access_token");
  token->refresh_token = string_of (object, "refresh_token");
  token->expires_at = expires_in > 0 ? now + expires_in : 0;

  /* The services answer a token the game is run with; Microsoft answers
   * the same shape with a refresh token as well. */
  if (token->access_token == NULL || *token->access_token == '\0')
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_PARSE,
                   "the service answered with no access token");
      aml_oauth_token_free (token);
      return NULL;
    }

  return token;
}

gboolean
aml_auth_polling_error (const char *json, gssize length, gboolean *slow_down,
                         GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *error_code = NULL;
  g_autofree char *said = NULL;
  JsonObject *object = NULL;

  g_return_val_if_fail (json != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (slow_down != NULL)
    *slow_down = FALSE;

  if (!open_object (json, length, &parser, error))
    return TRUE;

  object = json_node_get_object (json_parser_get_root (parser));
  error_code = string_of (object, "error");
  said = string_of (object, "error_description");

  if (error_code == NULL)
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_PARSE,
                   "the service refused the sign-in without saying why");
      return TRUE;
    }

  /* The user has not signed in yet: this is what every poll answers
     until they do, and it is not a failure. */
  if (g_str_equal (error_code, "authorization_pending"))
    return FALSE;

  /* The service asks to be asked less often, which is what a client that
     polls too fast is told. */
  if (g_str_equal (error_code, "slow_down"))
    {
      if (slow_down != NULL)
        *slow_down = TRUE;

      return FALSE;
    }

  if (g_str_equal (error_code, "authorization_declined"))
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_DECLINED,
                   "the sign-in was declined%s%s", said != NULL ? ": " : "",
                   said != NULL ? said : "");
      return TRUE;
    }

  if (g_str_equal (error_code, "expired_token"))
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_DECLINED,
                   "the code ran out before the sign-in was finished");
      return TRUE;
    }

  g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_DECLINED, "%s%s%s",
               error_code, said != NULL ? ": " : "",
               said != NULL ? said : "");

  return TRUE;
}

/* ---------------------------------------------------------- *
 * Xbox Live and XSTS
 * ---------------------------------------------------------- */

void
aml_xbox_token_free (AmlXboxToken *token)
{
  if (token == NULL)
    return;

  g_free (token->token);
  g_free (token->user_hash);
  g_free (token);
}

/* The words for the refusals XSTS answers in numbers. Every one of them is
 * something a user can be told and can do something about, which a number
 * is not. */
static const char *
xsts_refusal (gint64 code)
{
  switch (code)
    {
    case 2148916233:
      /* The Microsoft account has never been an Xbox one. */
      return "this Microsoft account has no Xbox profile behind it yet: "
             "sign in to xbox.com once with it, then sign in here again";

    case 2148916238:
      /* A child account, which the services do not serve. */
      return "this is a child account: it has to be added to a Microsoft "
             "family by an adult before it can play";

    case 2148916235:
    case 2148916236:
      return "Xbox Live is not available for the country this account is "
             "registered in";

    case 2148916222:
    case 2148916223:
      return "this account needs an adult to verify it before it can play, "
             "which is what South Korean accounts have to do";

    default:
      return NULL;
    }
}

AmlXboxToken *
aml_auth_xbox_token_from_json (const char *json, gssize length,
                               GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *message = NULL;
  AmlXboxToken *token = NULL;
  JsonObject *object = NULL;
  JsonNode *claims = NULL;
  gint64 xerr = 0;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  token = g_new0 (AmlXboxToken, 1);
  token->token = string_of (object, "Token");
  message = string_of (object, "Message");

  /* The hash of the user is in the display claims, and it is half of what
     the services are given to identify the player. */
  claims = json_object_get_member (object, "DisplayClaims");

  if (token->token != NULL && claims != NULL &&
      JSON_NODE_HOLDS_OBJECT (claims))
    {
      JsonNode *xui = json_object_get_member (json_node_get_object (claims),
                                             "xui");

      if (xui != NULL && JSON_NODE_HOLDS_ARRAY (xui))
        {
          JsonArray *array = json_node_get_array (xui);

          if (json_array_get_length (array) > 0)
            {
              JsonNode *first = json_array_get_element (array, 0);

              if (first != NULL && JSON_NODE_HOLDS_OBJECT (first))
                token->user_hash =
                  string_of (json_node_get_object (first), "uhs");
            }
        }
    }

  if (token->token != NULL && *token->token != '\0' &&
      token->user_hash != NULL && *token->user_hash != '\0')
    return token;

  /* A refusal: XSTS answers with a number and a sentence, and the number
     is the one that says what a user can do about it. */
  if (int_of (object, "XErr", &xerr))
    {
      const char *words = xsts_refusal (xerr);

      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_XBOX, "%s (XErr %"
                   G_GINT64_FORMAT ")%s%s", words != NULL ? words
                                                          : "Xbox services "
                                                            "refused the "
                                                            "sign-in",
                   xerr, message != NULL ? ": " : "",
                   message != NULL ? message : "");
    }
  else
    {
      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_XBOX,
                   "Xbox services answered with no token and no reason%s%s",
                   message != NULL ? ": " : "",
                   message != NULL ? message : "");
    }

  aml_xbox_token_free (token);

  return NULL;
}

char *
aml_auth_identity_token (const char *user_hash, const char *xsts_token)
{
  g_return_val_if_fail (user_hash != NULL, NULL);
  g_return_val_if_fail (xsts_token != NULL, NULL);

  /* The form the services document: what XSTS says the user is, and the
     token that says it. */
  return g_strdup_printf ("XBL3.0 x=%s;%s", user_hash, xsts_token);
}

/* ---------------------------------------------------------- *
 * The Minecraft services
 * ---------------------------------------------------------- */

AmlOAuthToken *
aml_auth_minecraft_token_from_json (const char *json, gssize length,
                                    gint64 now, GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *message = NULL;
  AmlOAuthToken *token = NULL;
  JsonObject *object = NULL;
  gint64 expires_in = 0;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  message = string_of (object, "errorMessage");

  token = g_new0 (AmlOAuthToken, 1);
  token->access_token = string_of (object, "access_token");
  int_of (object, "expires_in", &expires_in);
  token->expires_at = expires_in > 0 ? now + expires_in : 0;

  if (token->access_token != NULL && *token->access_token != '\0')
    return token;

  /* "Invalid app registration" is what the services answer an Azure
     application that is not registered for Minecraft with, and it is the
     one refusal here that is about this build rather than about the
     user. */
  g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_XBOX,
               "the Minecraft services refused the sign-in%s%s",
               message != NULL ? ": " : "",
               message != NULL ? message : "");

  aml_oauth_token_free (token);

  return NULL;
}

gboolean
aml_auth_owns_minecraft_from_json (const char *json, gssize length,
                                   GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  JsonNode *items = NULL;
  JsonArray *array = NULL;
  g_autofree char *message = NULL;

  g_return_val_if_fail (json != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!open_object (json, length, &parser, error))
    return FALSE;

  {
    JsonObject *object = json_node_get_object (json_parser_get_root (parser));

    /* The service has used both names over time. Current responses call the
     * collection "entitlements" and its entry field "name"; older
     * responses used "items" and "entitlementName". */
    items = json_object_get_member (object, "entitlements");
    if (items == NULL)
      items = json_object_get_member (object, "items");

    /* A document with no list in it is a refusal rather than a list of
       nothing, and the two say different things. */
    if (items == NULL || !JSON_NODE_HOLDS_ARRAY (items))
      {
        message = string_of (object, "errorMessage");

        g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_XBOX,
                     "the Minecraft services did not answer with what the "
                     "account owns%s%s", message != NULL ? ": " : "",
                     message != NULL ? message : "");
        return FALSE;
      }
  }

  array = json_node_get_array (items);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *item = json_array_get_element (array, i);
      g_autofree char *name = NULL;

      if (!JSON_NODE_HOLDS_OBJECT (item))
        continue;

       name = string_of (json_node_get_object (item), "name");
       if (name == NULL)
         name = string_of (json_node_get_object (item), "entitlementName");

      /* The store names it minecraft_java; the older names are still
         published by some accounts, so they count as owning it too. */
      if (name != NULL &&
          (g_str_equal (name, "minecraft_java") ||
           g_str_equal (name, "product_minecraft") ||
           g_str_equal (name, "game_minecraft")))
        return TRUE;
    }

  g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_NO_GAME,
               "this account does not own Minecraft: Java, which is what it "
               "would have to own to play it");

  return FALSE;
}

void
aml_minecraft_profile_free (AmlMinecraftProfile *profile)
{
  if (profile == NULL)
    return;

  g_free (profile->id);
  g_free (profile->name);
  g_free (profile->avatar_url);
  g_free (profile);
}

AmlMinecraftProfile *
aml_auth_profile_from_json (const char *json, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *message = NULL;
  AmlMinecraftProfile *profile = NULL;
  JsonNode *skins = NULL;
  JsonArray *array = NULL;
  JsonObject *object = NULL;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  profile = g_new0 (AmlMinecraftProfile, 1);
  profile->id = string_of (object, "id");
  profile->name = string_of (object, "name");

  if (profile->id == NULL || *profile->id == '\0' ||
      profile->name == NULL || *profile->name == '\0')
    {
      /* The services answer 404 with a document for an account that owns
         the game and has never played it: there is no profile to speak
         of yet, which is not the same as not owning it. */
      message = string_of (object, "errorMessage");

      g_set_error (error, AML_AUTH_ERROR, AML_AUTH_ERROR_NO_PROFILE,
                   "this account owns Minecraft: Java but has never played "
                   "it, so it has no profile yet%s%s",
                   message != NULL ? ": " : "",
                   message != NULL ? message : "");

      aml_minecraft_profile_free (profile);
      return NULL;
    }

  /* The skin that is worn is the one to show for the account. */
  skins = json_object_get_member (object, "skins");

  if (skins != NULL && JSON_NODE_HOLDS_ARRAY (skins))
    {
      array = json_node_get_array (skins);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *item = json_array_get_element (array, i);
          g_autofree char *state = NULL;
          g_autofree char *url = NULL;

          if (!JSON_NODE_HOLDS_OBJECT (item))
            continue;

          state = string_of (json_node_get_object (item), "state");
          url = string_of (json_node_get_object (item), "url");

          if (url != NULL && *url != '\0' &&
              (state == NULL || g_str_equal (state, "ACTIVE")))
            {
              profile->avatar_url = g_steal_pointer (&url);
              break;
            }
        }
    }

  return profile;
}

char *
aml_auth_profile_uuid (const char *id)
{
  g_autoptr (GString) out = NULL;

  g_return_val_if_fail (id != NULL, NULL);

  /* The services publish the id as 32 hex characters, and the game is
     given it dashed, which is the form every other account carries. */
  if (strlen (id) == 36 && id[8] == '-' && id[13] == '-' && id[18] == '-' &&
      id[23] == '-')
    return g_ascii_strdown (id, -1);

  if (strlen (id) != 32)
    return NULL;

  for (guint i = 0; i < 32; i++)
    {
      if (!g_ascii_isxdigit (id[i]))
        return NULL;
    }

  out = g_string_new (NULL);

  for (guint i = 0; i < 32; i++)
    {
      if (i == 8 || i == 12 || i == 16 || i == 20)
        g_string_append_c (out, '-');

      g_string_append_c (out, g_ascii_tolower (id[i]));
    }

  return g_string_free (g_steal_pointer (&out), FALSE);
}/* ---------------------------------------------------------- *
 * One request of the chain
 *
 * Every step is one request and one answer, and the step after it is
 * decided by what came back. Nothing is logged on the way: what these
 * answers hold are tokens, and a token is not something to write down.
 * ---------------------------------------------------------- */

typedef enum {
  STEP_DEVICE_CODE, /* asking for the code a user has to type */
  STEP_TOKEN,       /* waiting for the user, then taking the token */
  STEP_XBL,
  STEP_XSTS,
  STEP_MINECRAFT,
  STEP_ENTITLEMENTS,
  STEP_PROFILE
} Step;

typedef struct _AuthRequest AuthRequest;

struct _AuthRequest
{
  GTask *task;
  SoupMessage *message;
  guint poll_source;
  AmlTaskScope *scope;
  Step step;

  gint status;
  char *body;

  /* What the user is signing in with: either a code to wait on, or a
   * refresh token that trades for the first step again. */
  char *device_code;
  char *refresh_token;
  guint interval;  /* how often the token endpoint may be asked */
  gint64 deadline; /* when the code stops being of any use, 0 for none */
  gint64 now;      /* the moment the chain started, so that every expiry
                    * it reads counts from the same second */

  /* What the chain has gathered on the way. */
  char *microsoft_refresh_token; /* the one the account keeps */
  char *xbl_token;
  char *xbl_user_hash;
  char *xsts_token;
  char *mc_access_token;
  gint64 mc_expires_at;
};

static void request_free (gpointer data);

static void sent (SoupSession *session, GAsyncResult *result,
                  gpointer user_data);

static SoupSession *
auth_session (void)
{
  /* One session for the life of the process, like the download module's,
     which is why every function of this one has to be called from the
     main context it was first called from. */
  static SoupSession *session = NULL;

  if (session == NULL)
    session = soup_session_new_with_options (
        "timeout", (guint) AML_AUTH_TIMEOUT_SECONDS, "user-agent",
        AML_AUTH_USER_AGENT, NULL);

  return session;
}

/* A sign-in is the task: the request lives as long as the task does, and
   the task is what is answered when it is over. */
static AuthRequest *
request_new (AmlTaskScope *scope)
{
  AuthRequest *self = g_new0 (AuthRequest, 1);

  /* Borrowed: this request is the task's own data and the scope owns that
     task. The caller keeps the operation alive until its callback. */
  self->task = g_object_ref (aml_task_scope_task (scope));
  self->scope = scope;
  self->now = g_get_real_time () / 1000000;

  /* The cancellation of the task is this module's own: a sign-in that is
     cancelled stops waiting, and nothing is answered twice. */
  g_task_set_check_cancellable (self->task, FALSE);
  g_task_set_task_data (self->task, self, request_free);

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
  aml_task_scope_return_error_deferred (self->scope, error);
  g_clear_object (&self->task);
}

/* The other way out: what the chain was for, which is the caller's now.
   A step finishes with either a code to sign in with or an account, so
   what it hands back is what it is and how that is freed. */
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

  aml_task_scope_return_pointer_deferred (self->scope, result, free_result);
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
      !aml_task_scope_return_error_if_cancelled (self->scope))
    return FALSE;

  task = g_steal_pointer (&self->task);
  g_object_unref (task);

  return TRUE;
}

static void
device_code_answered (AuthRequest *self);
static void token_answered (AuthRequest *self);
static void xbl_answered (AuthRequest *self);
static void xsts_answered (AuthRequest *self);
static void minecraft_answered (AuthRequest *self);
static void entitlements_answered (AuthRequest *self);
static void profile_answered (AuthRequest *self);
static void ask_xbl (AuthRequest *self, const char *access_token);
static void ask_xsts (AuthRequest *self);
static void ask_minecraft (AuthRequest *self);
static void ask_entitlements (AuthRequest *self);
static void ask_profile (AuthRequest *self);

/* Sends one request and hands what came back to the step that asked for
 * it. The answer is kept as it arrived, whatever the status was: a
 * service that refuses something says why in the body of the refusal,
 * and that sentence is what a user is told. */
static void
send (AuthRequest         *self,
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

  soup_session_send_and_read_async (
      auth_session (), self->message, G_PRIORITY_DEFAULT,
      g_task_get_cancellable (self->task), (GAsyncReadyCallback) sent, self);
}

static void
sent (SoupSession         *session G_GNUC_UNUSED,
      GAsyncResult        *result,
      gpointer             user_data)
{
  /* The request belongs to the task: the last thing this callback does is
     answer the task, and nothing here touches the request after that. */
  AuthRequest *self = user_data;
  g_autoptr (GError) transport_error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  gsize length = 0;
  const char *data = NULL;

  self->status = soup_message_get_status (self->message);
  bytes = soup_session_send_and_read_finish (auth_session (), result,
                                             &transport_error);
  g_clear_object (&self->message);

  data = bytes != NULL ? g_bytes_get_data (bytes, &length) : NULL;
  g_free (self->body);
  self->body = data != NULL ? g_strndup (data, length) : g_strdup ("");

  /* A request that never came back is not a service that said no. */
  if (transport_error != NULL)
    {
      if (g_error_matches (transport_error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          answer_if_cancelled (self))
        return;

      fail (self,
            g_error_new (AML_AUTH_ERROR, AML_AUTH_ERROR_NETWORK,
                         "the sign-in service could not be reached: %s",
                         transport_error->message));
      return;
    }

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
static GError *
refusal (const char  *body,
         gint          status,
         AmlAuthError  code,
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

  return g_error_new (AML_AUTH_ERROR, code, "%s (HTTP %d)%s%s", what, status,
                      said != NULL && *said != '\0' ? ": " : "",
                      said != NULL ? said : "");
}

/* ---------------------------------------------------------- *
 * The first step: a code to sign in with
 * ---------------------------------------------------------- */

static void
device_code_answered (AuthRequest *self)
{
  g_autoptr (AmlDeviceCode) code = NULL;
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
    {
      fail (self,
            refusal (self->body, self->status, AML_AUTH_ERROR_DEVICE_CODE,
                     "Microsoft would not give a code to sign in with"));
      return;
    }

  code = aml_auth_device_code_from_json (self->body, -1, &error);

  if (code == NULL)
    fail (self, g_steal_pointer (&error));
  else
    succeed (self, g_steal_pointer (&code),
             (GDestroyNotify) aml_device_code_free);
}

AmlTaskScope *
aml_auth_device_code_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  g_autofree char *form = NULL;
  AuthRequest *self = NULL;

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  form = soup_form_encode ("client_id", AML_MICROSOFT_CLIENT_ID, "scope",
                           AML_MICROSOFT_SCOPE, NULL);

  send (self, AML_MICROSOFT_DEVICE_CODE_URL, SOUP_METHOD_POST, form,
        "application/x-www-form-urlencoded", NULL, STEP_DEVICE_CODE);

  return g_steal_pointer (&scope);
}

AmlDeviceCode *
aml_auth_device_code_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (AmlDeviceCode *) g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The second step: waiting for the user
 * ---------------------------------------------------------- */

/* The form the token endpoint is asked with, which is a different grant
 * depending on whether a code is being waited on or a token is being
 * refreshed. (transfer full) */
static char *
token_form (AuthRequest *self)
{
  if (self->refresh_token != NULL)
    return soup_form_encode ("client_id", AML_MICROSOFT_CLIENT_ID,
                             "grant_type", "refresh_token", "refresh_token",
                             self->refresh_token, "scope",
                             AML_MICROSOFT_SCOPE, NULL);

  return soup_form_encode ("client_id", AML_MICROSOFT_CLIENT_ID, "grant_type",
                           "urn:ietf:params:oauth:grant-type:device_code",
                           "device_code", self->device_code, "scope",
                           AML_MICROSOFT_SCOPE, NULL);
}

static void
ask_for_token (AuthRequest *self)
{
  g_autofree char *form = token_form (self);

  send (self, AML_MICROSOFT_TOKEN_URL, SOUP_METHOD_POST, form,
        "application/x-www-form-urlencoded", NULL, STEP_TOKEN);
}

/* The next poll, once the interval the service asked for has passed. */
static gboolean
poll_again (gpointer user_data)
{
  AuthRequest *self = user_data;
  guint source_id = self->poll_source;

  self->poll_source = 0;
  aml_task_scope_forget_source (self->scope, source_id);

  if (self->task == NULL || answer_if_cancelled (self))
    return G_SOURCE_REMOVE;

  ask_for_token (self);

  return G_SOURCE_REMOVE;
}

static void
token_answered (AuthRequest *self)
{
  g_autoptr (AmlOAuthToken) token = NULL;
  g_autoptr (GError) error = NULL;
  gboolean slow_down = FALSE;

  if (self->status == 200)
    {
      token = aml_auth_oauth_token_from_json (self->body, -1, self->now, &error);

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
  if (aml_auth_polling_error (self->body, -1, &slow_down, &error))
    {
      if (error == NULL)
        error = g_error_new_literal (AML_AUTH_ERROR, AML_AUTH_ERROR_PARSE,
                                     "the service refused the sign-in without "
                                     "saying why");

      fail (self, g_steal_pointer (&error));
      return;
    }

  /* A service that says the client is asking too fast is to be asked
     slower, and that is the whole of what the answer says. */
  if (slow_down)
    self->interval += 5;

  /* The code is good for a quarter of an hour and no longer, so a user
     who walked away is told rather than left waiting. */
  if (self->deadline > 0 &&
      g_get_real_time () / 1000000 >= self->deadline)
    {
      fail (self,
            g_error_new_literal (AML_AUTH_ERROR, AML_AUTH_ERROR_DECLINED,
                                 "the code ran out before the sign-in was "
                                 "finished: ask for a new one"));
      return;
    }

  self->poll_source = g_timeout_add_seconds (self->interval, poll_again, self);
  aml_task_scope_track_source (self->scope, self->poll_source);
}

/* ---------------------------------------------------------- *
 * The third step: Xbox Live
 * ---------------------------------------------------------- */

static void
ask_xbl (AuthRequest *self, const char *access_token)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *ticket = NULL;
  g_autofree char *body = NULL;
  const char *headers[] = { "x-xbl-contract-version",
                            AML_XBOX_CONTRACT_VERSION, "Accept",
                            "application/json", NULL };

  /* The Microsoft token is handed over as a ticket of the sign-in, which
     is how the Xbox services document the request. */
  ticket = g_strconcat ("d=", access_token, NULL);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "Properties");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "AuthMethod");
  json_builder_add_string_value (builder, "RPS");
  json_builder_set_member_name (builder, "SiteName");
  json_builder_add_string_value (builder, "user.auth.xboxlive.com");
  json_builder_set_member_name (builder, "RpsTicket");
  json_builder_add_string_value (builder, ticket);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "RelyingParty");
  json_builder_add_string_value (builder, "http://auth.xboxlive.com");
  json_builder_set_member_name (builder, "TokenType");
  json_builder_add_string_value (builder, "JWT");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  body = json_generator_to_data (generator, NULL);

  send (self, AML_XBOX_USER_URL, SOUP_METHOD_POST, body, "application/json",
        headers, STEP_XBL);
}

static void
xbl_answered (AuthRequest *self)
{
  g_autoptr (AmlXboxToken) token = NULL;
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
    {
      fail (self, refusal (self->body, self->status, AML_AUTH_ERROR_XBOX,
                           "Xbox Live refused the sign-in"));
      return;
    }

  token = aml_auth_xbox_token_from_json (self->body, -1, &error);

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
 * The fourth step: XSTS
 * ---------------------------------------------------------- */

static void
ask_xsts (AuthRequest *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *body = NULL;
  const char *headers[] = { "x-xbl-contract-version",
                            AML_XBOX_CONTRACT_VERSION, "Accept",
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
  json_builder_add_string_value (builder, AML_MINECRAFT_RELYING_PARTY);
  json_builder_set_member_name (builder, "TokenType");
  json_builder_add_string_value (builder, "JWT");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  body = json_generator_to_data (generator, NULL);

  send (self, AML_XBOX_XSTS_URL, SOUP_METHOD_POST, body, "application/json",
        headers, STEP_XSTS);
}

static void
xsts_answered (AuthRequest *self)
{
  g_autoptr (AmlXboxToken) token = NULL;
  g_autoptr (GError) error = NULL;

  /* A refusal of XSTS comes as a number and a sentence, and the number
     is the one that says what a user can do about it. */
  token = aml_auth_xbox_token_from_json (self->body, -1, &error);

  if (token == NULL)
    {
      fail (self, g_steal_pointer (&error));
      return;
    }

  if (self->status < 200 || self->status > 299)
    {
      fail (self, refusal (self->body, self->status, AML_AUTH_ERROR_XBOX,
                           "Xbox services refused the sign-in"));
      return;
    }

  g_free (self->xsts_token);
  self->xsts_token = g_steal_pointer (&token->token);

  /* The services are told who the player is by the hash the user token
     came with, and without it there is nobody to sign in as. */
  if (self->xbl_user_hash == NULL)
    {
      fail (self, g_error_new_literal (AML_AUTH_ERROR, AML_AUTH_ERROR_PARSE,
                                       "Xbox services answered with a token "
                                       "and no user"));
      return;
    }

  ask_minecraft (self);
}

/* ---------------------------------------------------------- *
 * The fifth step: the Minecraft services
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

  /* Current Minecraft services use the launcher login contract. Prism
   * Launcher sends the XBL3 token and explicitly identifies the PC launcher;
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

  send (self, AML_MINECRAFT_SERVICES_URL "/launcher/login",
        SOUP_METHOD_POST, body, "application/json", headers, STEP_MINECRAFT);
}

static void
minecraft_answered (AuthRequest *self)
{
  g_autoptr (AmlOAuthToken) token = NULL;
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
    {
      /* "Invalid app registration" is what the services answer an Azure
         application that is not registered for Minecraft with, and it
         comes through in the sentence they give. */
      fail (self, refusal (self->body, self->status, AML_AUTH_ERROR_XBOX,
                           "the Minecraft services refused the sign-in"));
      return;
    }

  token = aml_auth_minecraft_token_from_json (self->body, -1,
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
 * The sixth step: whether the account owns the game
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

  /* Prism and the current Minecraft services use the license endpoint with
     a request id. The older mcstore endpoint can return a legacy-shaped
     response or reject otherwise valid accounts. */
  url = g_strdup_printf (AML_MINECRAFT_SERVICES_URL
                         "/entitlements/license?requestId=%s", request_id);
  send (self, url,
        SOUP_METHOD_GET, NULL, NULL, headers, STEP_ENTITLEMENTS);
}

static void
entitlements_answered (AuthRequest *self)
{
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
    {
      fail (self, refusal (self->body, self->status, AML_AUTH_ERROR_XBOX,
                           "the Minecraft services did not answer with what "
                           "this account owns"));
      return;
    }

  if (!aml_auth_owns_minecraft_from_json (self->body, -1, &error))
    fail (self, g_steal_pointer (&error));
  else
    ask_profile (self);
}

/* ---------------------------------------------------------- *
 * The seventh step: who the player is
 * ---------------------------------------------------------- */

static void
ask_profile (AuthRequest *self)
{
  g_autofree char *authorization = NULL;
  const char *headers[] = { "Authorization", NULL, "Accept",
                            "application/json", NULL };

  authorization = g_strconcat ("Bearer ", self->mc_access_token, NULL);
  headers[1] = authorization;

  send (self, AML_MINECRAFT_SERVICES_URL "/minecraft/profile",
        SOUP_METHOD_GET, NULL, NULL, headers, STEP_PROFILE);
}

static void
profile_answered (AuthRequest *self)
{
  g_autoptr (AmlMinecraftProfile) profile = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = NULL;
  AmlAccount *account = NULL;

  profile = aml_auth_profile_from_json (self->body, -1, &error);

  if (profile == NULL)
    {
      /* The services answer 404 for an account that owns the game and has
         never played it, which the parser says in its own words rather
         than as a service that could not be reached. */
      fail (self, g_steal_pointer (&error));
      return;
    }

  if (self->status < 200 || self->status > 299)
    {
      fail (self, refusal (self->body, self->status, AML_AUTH_ERROR_NO_PROFILE,
                           "the Minecraft services have no profile for this "
                           "account"));
      return;
    }

  uuid = aml_auth_profile_uuid (profile->id);

  if (uuid == NULL)
    {
      fail (self,
            g_error_new (AML_AUTH_ERROR, AML_AUTH_ERROR_PARSE,
                         "the Minecraft services answered with a profile id "
                         "that is not a uuid"));
      return;
    }

  /* What the launcher is handed: who the player is, and the token it is
     run with. Nothing else of the chain is kept. */
  account = g_new0 (AmlAccount, 1);
  account->type = AML_ACCOUNT_MICROSOFT;
  account->id = g_strdup (profile->id);
  account->name = g_strdup (profile->name);
  account->uuid = g_steal_pointer (&uuid);
  account->access_token = g_strdup (self->mc_access_token);
  account->expires_at = self->mc_expires_at;
  account->refresh_token = g_strdup (self->microsoft_refresh_token);
  account->avatar_url = g_strdup (profile->avatar_url);

  succeed (self, account, (GDestroyNotify) aml_account_free);
}

/* ---------------------------------------------------------- *
 * Signing in, and refreshing
 * ---------------------------------------------------------- */

AmlTaskScope *
aml_auth_sign_in_async (const AmlDeviceCode   *code,
                        GAsyncReadyCallback    callback,
                        gpointer               user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  AuthRequest *self = NULL;

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

AmlAccount *
aml_auth_sign_in_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (AmlAccount *) g_task_propagate_pointer (G_TASK (result), error);
}

AmlTaskScope *
aml_auth_refresh_async (const AmlAccount   *account,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  AuthRequest *self = NULL;

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
            g_error_new (AML_AUTH_ERROR, AML_AUTH_ERROR_DECLINED,
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

AmlAccount *
aml_auth_refresh_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (AmlAccount *) g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Letting go
 *
 * The request is the data of the task, so it is freed when the task is:
 * the caller drops the task when it has been answered and this frees
 * what was gathered on the way, tokens and all.
 * ---------------------------------------------------------- */

static void
request_free (gpointer data)
{
  AuthRequest *self = data;

  /* The scope owns the task this request is the data of, so reaching here
     is the scope going away: it has already released the sources the poll
     was waiting on. */

  g_clear_object (&self->message);
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
