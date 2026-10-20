/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Microsoft, Xbox and Minecraft response parsers. The asynchronous
 * authentication chain remains in nuvasha-auth-microsoft.c. */

#include "nuvasha-auth-microsoft.h"
#include "nuvasha-i18n.h"

#include <string.h>

/* ---------------------------------------------------------- *
 * Reading an answer
 * ---------------------------------------------------------- */

/* Parse @json and return its root object. */
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
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_PARSE,
                   "the service answered something that is not a document: "
                   "%s", local_error->message);
      g_clear_object (parser);
      return FALSE;
    }

  root = json_parser_get_root (*parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_PARSE,
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
nuvasha_device_code_free (NuvashaDeviceCode *code)
{
  if (code == NULL)
    return;

  g_free (code->user_code);
  g_free (code->verification_uri);
  g_free (code->message);
  g_free (code->device_code);
  g_free (code);
}

NuvashaDeviceCode *
nuvasha_auth_device_code_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *user_code = NULL;
  g_autofree char *verification_uri = NULL;
  g_autofree char *message = NULL;
  g_autofree char *device_code = NULL;
  NuvashaDeviceCode *code = NULL;
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
  /* Do not allow a service response to suspend the main-loop poll for an
   * effectively unbounded time. */
  if (!int_of (object, "interval", &interval) || interval < 1 ||
      interval > 3600)
    interval = 5;

  /* A code without a place to type it, or without a code to type, is
     nothing a user could sign in with. The device code itself is what the
     chain polls with and the caller does not have to see it. */
  if (user_code == NULL || *user_code == '\0' ||
      verification_uri == NULL || *verification_uri == '\0' ||
      device_code == NULL || *device_code == '\0')
    {
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_DEVICE_CODE,
                   "Microsoft did not answer with a code to sign in with");
      return NULL;
    }

  code = g_new0 (NuvashaDeviceCode, 1);
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
nuvasha_oauth_token_free (NuvashaOAuthToken *token)
{
  if (token == NULL)
    return;

  g_free (token->access_token);
  g_free (token->refresh_token);
  g_free (token);
}

NuvashaOAuthToken *
nuvasha_auth_oauth_token_from_json (const char *json, gssize length, gint64 now,
                                GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  NuvashaOAuthToken *token = NULL;
  JsonObject *object = NULL;
  gint64 expires_in = 0;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  int_of (object, "expires_in", &expires_in);

  token = g_new0 (NuvashaOAuthToken, 1);
  token->access_token = string_of (object, "access_token");
  token->refresh_token = string_of (object, "refresh_token");
  token->expires_at = (expires_in > 0 &&
                       now <= G_MAXINT64 - expires_in)
                        ? now + expires_in
                        : (expires_in > 0 ? G_MAXINT64 : 0);

  /* The services answer a token the game is run with; Microsoft answers
   * the same shape with a refresh token as well. */
  if (token->access_token == NULL || *token->access_token == '\0')
    {
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_PARSE,
                   "the service answered with no access token");
      nuvasha_oauth_token_free (token);
      return NULL;
    }

  return token;
}

gboolean
nuvasha_auth_polling_error (const char *json, gssize length, gboolean *slow_down,
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
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_PARSE,
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
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_DECLINED,
                   "the sign-in was declined%s%s", said != NULL ? ": " : "",
                   said != NULL ? said : "");
      return TRUE;
    }

  if (g_str_equal (error_code, "expired_token"))
    {
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_DECLINED,
                   "the code ran out before the sign-in was finished");
      return TRUE;
    }

  g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_DECLINED, "%s%s%s",
               error_code, said != NULL ? ": " : "",
               said != NULL ? said : "");

  return TRUE;
}

/* ---------------------------------------------------------- *
 * Xbox Live and XSTS
 * ---------------------------------------------------------- */

void
nuvasha_xbox_token_free (NuvashaXboxToken *token)
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
      return _("this Microsoft account has no Xbox profile behind it yet: "
               "sign in to xbox.com once with it, then sign in here again");

    case 2148916238:
      /* A child account, which the services do not serve. */
      return _("this is a child account: it has to be added to a Microsoft "
               "family by an adult before it can play");

    case 2148916235:
    case 2148916236:
      return _("Xbox Live is not available for the country this account is "
               "registered in");

    case 2148916222:
    case 2148916223:
      return _("this account needs an adult to verify it before it can play, "
               "which is what South Korean accounts have to do");

    default:
      return NULL;
    }
}

NuvashaXboxToken *
nuvasha_auth_xbox_token_from_json (const char *json, gssize length,
                               GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *message = NULL;
  NuvashaXboxToken *token = NULL;
  JsonObject *object = NULL;
  JsonNode *claims = NULL;
  gint64 xerr = 0;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  token = g_new0 (NuvashaXboxToken, 1);
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

      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_XBOX, "%s (XErr %"
                   G_GINT64_FORMAT ")%s%s", words != NULL ? words
                                                          : "Xbox services "
                                                            "refused the "
                                                            "sign-in",
                   xerr, message != NULL ? ": " : "",
                   message != NULL ? message : "");
    }
  else
    {
      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_XBOX,
                   "Xbox services answered with no token and no reason%s%s",
                   message != NULL ? ": " : "",
                   message != NULL ? message : "");
    }

  nuvasha_xbox_token_free (token);

  return NULL;
}

char *
nuvasha_auth_identity_token (const char *user_hash, const char *xsts_token)
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

NuvashaOAuthToken *
nuvasha_auth_minecraft_token_from_json (const char *json, gssize length,
                                    gint64 now, GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *message = NULL;
  NuvashaOAuthToken *token = NULL;
  JsonObject *object = NULL;
  gint64 expires_in = 0;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  message = string_of (object, "errorMessage");

  token = g_new0 (NuvashaOAuthToken, 1);
  token->access_token = string_of (object, "access_token");
  int_of (object, "expires_in", &expires_in);
  token->expires_at = (expires_in > 0 &&
                       now <= G_MAXINT64 - expires_in)
                        ? now + expires_in
                        : (expires_in > 0 ? G_MAXINT64 : 0);

  if (token->access_token != NULL && *token->access_token != '\0')
    return token;

  /* "Invalid app registration" is what the services answer an Azure
     application that is not registered for Minecraft with, and it is the
     one refusal here that is about this build rather than about the
     user. */
  g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_XBOX,
               "the Minecraft services refused the sign-in%s%s",
               message != NULL ? ": " : "",
               message != NULL ? message : "");

  nuvasha_oauth_token_free (token);

  return NULL;
}

gboolean
nuvasha_auth_owns_minecraft_from_json (const char *json, gssize length,
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

        g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_XBOX,
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

  g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_NO_GAME,
               "this account does not own Minecraft: Java, which is what it "
               "would have to own to play it");

  return FALSE;
}

void
nuvasha_minecraft_profile_free (NuvashaMinecraftProfile *profile)
{
  if (profile == NULL)
    return;

  g_free (profile->id);
  g_free (profile->name);
  g_free (profile->avatar_url);
  g_free (profile);
}

NuvashaMinecraftProfile *
nuvasha_auth_profile_from_json (const char *json, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *message = NULL;
  NuvashaMinecraftProfile *profile = NULL;
  JsonNode *skins = NULL;
  JsonArray *array = NULL;
  JsonObject *object = NULL;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!open_object (json, length, &parser, error))
    return NULL;

  object = json_node_get_object (json_parser_get_root (parser));
  profile = g_new0 (NuvashaMinecraftProfile, 1);
  profile->id = string_of (object, "id");
  profile->name = string_of (object, "name");

  if (profile->id == NULL || *profile->id == '\0' ||
      profile->name == NULL || *profile->name == '\0')
    {
      /* The services answer 404 with a document for an account that owns
         the game and has never played it: there is no profile to speak
         of yet, which is not the same as not owning it. */
      message = string_of (object, "errorMessage");

      g_set_error (error, nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_NO_PROFILE,
                   "this account owns Minecraft: Java but has never played "
                   "it, so it has no profile yet%s%s",
                   message != NULL ? ": " : "",
                   message != NULL ? message : "");

      nuvasha_minecraft_profile_free (profile);
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
nuvasha_auth_profile_uuid (const char *id)
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
}
