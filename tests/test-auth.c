/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvashalauncher-auth-microsoft: what nuvashalauncher reads out of what each service
 * of the sign-in answered.
 *
 * The chain is seven requests in a row, and every one of them answers
 * either with something to go on with or with a refusal. What has to be
 * right is reading the code, tokens, user, account ownership, player profile,
 * and useful error messages rather than raw service status codes.
 *
 * The answers come from the fixtures in tests/fixtures, in the shape the
 * services document, so nothing here needs the network and nothing waits
 * for a browser. */

#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-auth-microsoft-private.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  GMainLoop *loop;
  nuvashalauncherTaskScope *scope;
  GError *error;
} AuthWait;

typedef struct
{
  GMainLoop *loop;
  nuvashalauncherTaskScope *scope;
  nuvashalauncherAccount *account;
  GError *error;
  gboolean refresh;
} AuthSuccessWait;

typedef void (*AuthAnswerFunc) (AuthRequest *);

static char *fixture (const char *name);

static void
auth_success_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                       gpointer user_data)
{
  AuthSuccessWait *wait = user_data;

  if (wait->refresh)
    wait->account = nuvashalauncher_auth_refresh_finish (result, &wait->error);
  else
    wait->account = nuvashalauncher_auth_sign_in_finish (result, &wait->error);

  nuvashalauncher_task_scope_unref (wait->scope);
  wait->scope = NULL;
  g_main_loop_quit (wait->loop);
}

static void
auth_fixture_transport (AuthRequest *self, const char *url, const char *method,
                         const char *body, const char **headers, Step step,
                         gpointer user_data G_GNUC_UNUSED)
{
  const char *name = NULL;

  g_assert_cmpstr (method, ==, step == STEP_TOKEN ? SOUP_METHOD_POST :
                   step == STEP_PROFILE || step == STEP_ENTITLEMENTS ? SOUP_METHOD_GET :
                   SOUP_METHOD_POST);
  g_assert_nonnull (body || step == STEP_PROFILE || step == STEP_ENTITLEMENTS);
  g_assert_nonnull (headers || step == STEP_TOKEN || step == STEP_PROFILE ||
                    step == STEP_ENTITLEMENTS);

  switch (step)
    {
    case STEP_TOKEN:
      g_assert_cmpstr (url, ==, nuvashalauncher_MICROSOFT_TOKEN_URL);
      name = "auth-token-ok.json";
      break;
    case STEP_XBL:
      g_assert_cmpstr (url, ==, nuvashalauncher_XBOX_USER_URL);
      name = "auth-xbl-user.json";
      break;
    case STEP_XSTS:
      g_assert_cmpstr (url, ==, nuvashalauncher_XBOX_XSTS_URL);
      name = "auth-xsts-ok.json";
      break;
    case STEP_MINECRAFT:
      g_assert_cmpstr (url, ==, nuvashalauncher_MINECRAFT_SERVICES_URL "/launcher/login");
      name = "auth-minecraft-token.json";
      break;
    case STEP_ENTITLEMENTS:
      g_assert_true (g_str_has_prefix (url, nuvashalauncher_MINECRAFT_SERVICES_URL
                                       "/entitlements/license?requestId="));
      name = "auth-entitlements-owned.json";
      break;
    case STEP_PROFILE:
      g_assert_cmpstr (url, ==, nuvashalauncher_MINECRAFT_SERVICES_URL "/minecraft/profile");
      name = "auth-profile.json";
      break;
    default:
      g_assert_not_reached ();
    }

  /* The real response reader replaces the previous response body before
   * dispatching the next state. Keep the fixture transport's ownership
   * semantics identical to that boundary. */
  g_free (self->body);
  self->body = NULL;
  self->step = step;
  self->status = 200;
  self->body = fixture (name);

  switch (step)
    {
    case STEP_TOKEN: token_answered (self); break;
    case STEP_XBL: xbl_answered (self); break;
    case STEP_XSTS: xsts_answered (self); break;
    case STEP_MINECRAFT: minecraft_answered (self); break;
    case STEP_ENTITLEMENTS: entitlements_answered (self); break;
    case STEP_PROFILE: profile_answered (self); break;
    default: g_assert_not_reached ();
    }
}

static void
auth_error_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  AuthWait *wait = user_data;

  g_assert_false (g_task_propagate_boolean (G_TASK (result), &wait->error));
  nuvashalauncher_task_scope_unref (wait->scope);
  wait->scope = NULL;
  g_main_loop_quit (wait->loop);
}

static void
run_auth_error_answer (const char *fixture_name, gint status,
                       AuthAnswerFunc answer, nuvashalauncherAuthError code,
                       const char *message)
{
  g_autofree char *body = fixture (fixture_name);
  AuthWait wait = { 0 };
  AuthRequest *request = NULL;

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.scope = nuvashalauncher_task_scope_new (NULL, NULL,
                                               auth_error_finished, &wait);
  request = request_new (wait.scope);
  request->status = status;
  request->body = g_strdup (body);
  answer (request);
  g_main_loop_run (wait.loop);

  g_assert_null (wait.scope);
  g_assert_error (wait.error, nuvashalauncher_AUTH_ERROR, code);
  g_assert_nonnull (strstr (wait.error->message, message));
  g_clear_error (&wait.error);
  g_main_loop_unref (wait.loop);
}

#ifndef nuvashalauncher_TEST_FIXTURES
#define nuvashalauncher_TEST_FIXTURES "tests/fixtures"
#endif

/* The document of a fixture, which is what every answer of the chain is
   read out of. (transfer full) */
static char *
fixture (const char *name)
{
  g_autofree char *path = g_build_filename (nuvashalauncher_TEST_FIXTURES, name, NULL);
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;

  if (!g_file_get_contents (path, &json, NULL, &error))
    g_error ("the fixture \"%s\" cannot be read: %s", name, error->message);

  return g_steal_pointer (&json);
}

/* ---------------------------------------------------------- *
 * The code a user is asked to type
 * ---------------------------------------------------------- */

static void
test_device_code (void)
{
  g_autofree char *json = fixture ("auth-device-code.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherDeviceCode) code = NULL;

  code = nuvashalauncher_auth_device_code_from_json (json, -1, &error);

  g_assert_no_error (error);
  g_assert_nonnull (code);

  g_assert_cmpstr (code->user_code, ==, "ABCDE-FGHIJ");
  g_assert_cmpstr (code->verification_uri, ==,
                   "https://microsoft.com/devicelogin");

  /* What the chain polls with is there as well: the dialog shows the code
     the user types and the chain needs the other half of the pair. */
  g_assert_cmpstr (code->device_code, ==,
                   "AQCTZn0h9aJ3nM4Kd8pQ2r7tUvWxYz");

  /* How long it is good for and how often it may be asked about. */
  g_assert_cmpuint (code->expires_in, ==, 900);
  g_assert_cmpuint (code->interval, ==, 5);

  /* What Microsoft says about it is what a user is shown: it is written
     for this exact moment and names the code. */
  g_assert_nonnull (code->message);
  g_assert_nonnull (strstr (code->message, "ABCDE-FGHIJ"));
}

/* A service that refuses says what is wrong in the document, and a
   document that is not one is not a sign-in anybody can do. */
static void
test_device_code_errors (void)
{
  g_autofree char *error_json = fixture ("auth-device-code-error.json");
  g_autofree char *not_a_document = g_strdup ("this is not a document");
  g_autofree char *no_place =
    g_strdup ("{\"user_code\":\"ABCDE-FGHIJ\",\"device_code\":\"x\"}");
  g_autofree char *bare = g_strdup ("{\"user_code\":\"ABC\",\"device_code\":\"d\","
                                    "\"verification_uri\":"
                                    "\"https://microsoft.com/x\"}");
  g_autoptr (GError) error = NULL;

  /* A refusal is not a code: there is nothing for a user to type. */
  g_assert_null (
      nuvashalauncher_auth_device_code_from_json (error_json, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_DEVICE_CODE);
  g_assert_nonnull (strstr (error->message, "code to sign in with"));
  g_clear_error (&error);

  /* Neither is a document that is not one. */
  g_assert_null (nuvashalauncher_auth_device_code_from_json (not_a_document, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_PARSE);
  g_clear_error (&error);

  /* A code without a place to type it would be nothing a user could do
     with, so it is refused rather than shown empty. */
  g_assert_null (nuvashalauncher_auth_device_code_from_json (no_place, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_DEVICE_CODE);
  g_clear_error (&error);

  /* The interval a service does not give is one nuvashalauncher asks about every five
     seconds, which is what the device code flow documents. */
  {
    g_autoptr (nuvashalauncherDeviceCode) code =
      nuvashalauncher_auth_device_code_from_json (bare, -1, &error);

    g_assert_no_error (error);
    g_assert_nonnull (code);
    g_assert_cmpuint (code->interval, ==, 5);
    g_assert_cmpuint (code->expires_in, >, 0);
  }

  g_print ("ok - the code a user types, and the refusals that are not one\n");
}

static void
test_token (void)
{
  g_autofree char *json = fixture ("auth-token-ok.json");
  const gint64 now = 1800000000;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherOAuthToken) token = NULL;

  token = nuvashalauncher_auth_oauth_token_from_json (json, -1, now, &error);

  g_assert_no_error (error);
  g_assert_nonnull (token);
  g_assert_cmpstr (token->access_token, ==,
                   "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiJ9.microsoft.access.token");

  /* The refresh token is what the account keeps, so the sign-in outlives
     the token it starts with. */
  g_assert_cmpstr (token->refresh_token, ==, "M.R3_BAY.the-refresh-token");

  /* The moment the token stops being good is counted from the moment the
     chain started, not from whenever a test happens to run. */
  g_assert_cmpint (token->expires_at, ==, now + 3599);

  g_print ("ok - the token endpoint answers a token with its expiry\n");
}

/* Every poll answers that the user has not signed in yet until they have,
   and that is the wait and not a failure. What ends the wait is a
   refusal, and each of those says what it is. */
static void
test_polling (void)
{
  g_autofree char *pending = fixture ("auth-token-pending.json");
  g_autofree char *slow_down = fixture ("auth-token-slow-down.json");
  g_autofree char *declined = fixture ("auth-token-declined.json");
  g_autofree char *expired = fixture ("auth-token-expired.json");
  g_autoptr (GError) error = NULL;
  gboolean slow = TRUE;

  /* The user is in the browser: the answer is that they have not signed
     in yet, and there is no error in it. */
  g_assert_false (nuvashalauncher_auth_polling_error (pending, -1, &slow, &error));
  g_assert_no_error (error);
  g_assert_false (slow);

  /* A service that says the client is asking too fast is to be asked
     slower, and that is the whole of what it says. */
  g_assert_false (nuvashalauncher_auth_polling_error (slow_down, -1, &slow, &error));
  g_assert_no_error (error);
  g_assert_true (slow);

  g_assert_true (nuvashalauncher_auth_polling_error (declined, -1, &slow, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_DECLINED);
  g_assert_nonnull (strstr (error->message, "declined"));
  g_clear_error (&error);

  /* A code that ran out ends it too, and says to ask for a new one. */
  g_assert_true (nuvashalauncher_auth_polling_error (expired, -1, &slow, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_DECLINED);
  g_assert_nonnull (strstr (error->message, "ran out"));
  g_clear_error (&error);

  /* A refusal nobody can explain is still a refusal, and not a document
     that was never one. */
  {
    g_autofree char *unknown = g_strdup ("{\"error\":\"something_new\"}");

    g_assert_true (nuvashalauncher_auth_polling_error (unknown, -1, &slow, &error));
    g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_DECLINED);
    g_assert_nonnull (strstr (error->message, "something_new"));
    g_clear_error (&error);
  }

  g_print ("ok - every poll of the token endpoint is read for what it is\n");
}

/* ---------------------------------------------------------- *
 * Xbox Live and XSTS
 * ---------------------------------------------------------- */

static void
test_xbox_token (void)
{
  g_autofree char *user = fixture ("auth-xbl-user.json");
  g_autofree char *xsts = fixture ("auth-xsts-ok.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherXboxToken) from_user = NULL;
  g_autoptr (nuvashalauncherXboxToken) from_xsts = NULL;
  g_autofree char *identity = NULL;

  from_user = nuvashalauncher_auth_xbox_token_from_json (user, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (from_user);
  g_assert_cmpstr (from_user->token, ==,
                   "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.xbl.user.token");
  g_assert_cmpstr (from_user->user_hash, ==,
                   "8535f6e5f7c14b9dbd6d1c48a4c1b8a3");

  from_xsts = nuvashalauncher_auth_xbox_token_from_json (xsts, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (from_xsts);
  g_assert_cmpstr (from_xsts->token, ==,
                   "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.xsts.token");
  g_assert_cmpstr (from_xsts->user_hash, ==, from_user->user_hash);

  /* What the services are given is the hash and the token in the form
     they document. */
  identity = nuvashalauncher_auth_identity_token (from_xsts->user_hash, from_xsts->token);
  g_assert_cmpstr (identity, ==,
                   "XBL3.0 x=8535f6e5f7c14b9dbd6d1c48a4c1b8a3;"
                   "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.xsts.token");

  g_print ("ok - Xbox Live and XSTS answer a token and the user it is for\n");
}

static void
test_xsts_refusals (void)
{
  g_autofree char *no_profile = fixture ("auth-xsts-no-xbox-profile.json");
  g_autofree char *child = fixture ("auth-xsts-child-account.json");
  g_autofree char *country = fixture ("auth-xsts-country.json");
  g_autofree char *unknown =
    g_strdup ("{\"XErr\":2148916222,\"Message\":\"South Korea\"}");
  g_autoptr (GError) error = NULL;

  /* A Microsoft account that has never been an Xbox one: what a user can
     do about it is sign in to xbox.com once. */
  g_assert_null (nuvashalauncher_auth_xbox_token_from_json (no_profile, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX);
  g_assert_nonnull (strstr (error->message, "xbox.com"));
  g_assert_nonnull (strstr (error->message, "2148916233"));
  g_clear_error (&error);

  /* A child account has to be added to a family before it can play. */
  g_assert_null (nuvashalauncher_auth_xbox_token_from_json (child, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX);
  g_assert_nonnull (strstr (error->message, "family"));
  g_assert_nonnull (strstr (error->message, "2148916238"));
  g_clear_error (&error);

  /* An account from a country the services do not serve says so. */
  g_assert_null (nuvashalauncher_auth_xbox_token_from_json (country, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX);
  g_assert_nonnull (strstr (error->message, "country"));
  g_clear_error (&error);

  /* A number nuvashalauncher does not know is still said as a number, with what the
     service said about it: a launcher must not invent a reason. */
  g_assert_null (nuvashalauncher_auth_xbox_token_from_json (unknown, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX);
  g_assert_nonnull (strstr (error->message, "2148916222"));
  g_assert_nonnull (strstr (error->message, "South Korea"));
  g_clear_error (&error);

  /* A document with no token and no number is a service that could not be
     read, which is not a refusal with a reason in it. */
  {
    g_autofree char *nothing = g_strdup ("{\"Token\":\"\"}");

    g_assert_null (nuvashalauncher_auth_xbox_token_from_json (nothing, -1, &error));
    g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX);
    g_clear_error (&error);
  }

  g_print ("ok - a refusal of XSTS is said in words and not in numbers\n");
}

/* ---------------------------------------------------------- *
 * The Minecraft services
 * ---------------------------------------------------------- */

static void
test_minecraft_token (void)
{
  g_autofree char *json = fixture ("auth-minecraft-token.json");
  g_autofree char *refused = fixture ("auth-minecraft-app-not-registered.json");
  const gint64 now = 1800000000;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherOAuthToken) token = NULL;

  token = nuvashalauncher_auth_minecraft_token_from_json (json, -1, now, &error);
  g_assert_no_error (error);
  g_assert_nonnull (token);
  g_assert_cmpstr (token->access_token, ==,
                   "eyJhbGciOiJIUzI1NiJ9.minecraft.services.token");
  g_assert_cmpint (token->expires_at, ==, now + 86400);

  /* The services have no refresh token of their own: what the account
     keeps is the one Microsoft gave. */
  g_assert_null (token->refresh_token);

  /* "Invalid app registration" is what the services answer an Azure
     application that is not registered for Minecraft with, and it comes
     through in their own words rather than as a status code. */
  g_assert_null (nuvashalauncher_auth_minecraft_token_from_json (refused, -1, now, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX);
  g_assert_nonnull (strstr (error->message, "Invalid app registration"));
  g_clear_error (&error);

  g_print ("ok - the services answer the token the game is run with\n");
}

/* What the account owns is asked before the profile is: an account that
   does not own the game cannot play it, and that is worth saying rather
   than showing a sign-in that appeared to work. */
static void
test_entitlements (void)
{
  g_autofree char *owned = fixture ("auth-entitlements-owned.json");
  g_autofree char *not_owned = fixture ("auth-entitlements-not-owned.json");
  g_autofree char *refused = fixture ("auth-entitlements-error.json");
  g_autoptr (GError) error = NULL;

  /* Owning it, under the name the store gives it now or the names it gave
     it before, is what a launch is allowed to start. */
  g_assert_true (nuvashalauncher_auth_owns_minecraft_from_json (owned, -1, &error));
  g_assert_no_error (error);

  /* Current Minecraft services responses use entitlements/name rather
   * than the older items/entitlementName shape. */
  {
    const char *current =
      "{\"entitlements\":[{\"name\":\"product_minecraft\"}]}";

    g_assert_true (nuvashalauncher_auth_owns_minecraft_from_json (current, -1, &error));
    g_assert_no_error (error);
  }

  /* Owning something else is not owning the game: an account of a family
     that only bought the game says what is missing. */
  g_assert_false (nuvashalauncher_auth_owns_minecraft_from_json (not_owned, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_GAME);
  g_assert_nonnull (strstr (error->message, "Minecraft: Java"));
  g_clear_error (&error);

  /* A document with no list in it is a refusal rather than a list of
     nothing, and the two say different things. */
  g_assert_false (nuvashalauncher_auth_owns_minecraft_from_json (refused, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_XBOX);
  g_assert_nonnull (strstr (error->message, "The user token is invalid"));
  g_clear_error (&error);

  /* A list of nothing at all is still not owning it. */
  {
    g_autofree char *empty = g_strdup ("{\"items\":[]}");

    g_assert_false (nuvashalauncher_auth_owns_minecraft_from_json (empty, -1, &error));
    g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_GAME);
    g_clear_error (&error);
  }

  g_print ("ok - what the account owns says whether a launch can happen\n");
}

static void
test_profile (void)
{
  g_autofree char *json = fixture ("auth-profile.json");
  g_autofree char *bare = fixture ("auth-profile-no-skin.json");
  g_autofree char *not_found = fixture ("auth-profile-not-found.json");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherMinecraftProfile) profile = NULL;
  g_autofree char *uuid = NULL;

  profile = nuvashalauncher_auth_profile_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (profile);
  g_assert_cmpstr (profile->id, ==, "069a79f444e94726a5befca90e38aaf5");
  g_assert_cmpstr (profile->name, ==, "Notch");

  /* The skin that is worn is the one to show for the account. */
  g_assert_cmpstr (profile->avatar_url, ==,
                   "https://textures.minecraft.net/texture/"
                   "0f0d5a2b1c4d6e8f0a2b4c6d8e0f1a3b5c7d9e0f2a4b6c8");

  /* The game is given the id dashed, which is the form every account
     carries and the one the offline UUID is in as well. */
  uuid = nuvashalauncher_auth_profile_uuid (profile->id);
  g_assert_cmpstr (uuid, ==, "069a79f4-44e9-4726-a5be-fca90e38aaf5");

  /* An id that is already dashed is taken as it is, in lower case. */
  {
    g_autofree char *dashed = nuvashalauncher_auth_profile_uuid ("069A79F4-44E9-4726-A5BE-FCA90E38AAF5");

    g_assert_cmpstr (dashed, ==, "069a79f4-44e9-4726-a5be-fca90e38aaf5");
  }

  /* An id that is not a uuid is not one, and nuvashalauncher does not turn it into
     one by guessing. */
  g_assert_null (nuvashalauncher_auth_profile_uuid ("not-a-uuid"));
  g_assert_null (nuvashalauncher_auth_profile_uuid ("069a79f444e94726a5befca90e38aaf"));

  /* An account that owns the game and has never played it has no profile
     yet, which is not the same as not owning it. */
  g_assert_null (nuvashalauncher_auth_profile_from_json (not_found, -1, &error));
  g_assert_error (error, nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_NO_PROFILE);
  g_assert_nonnull (strstr (error->message, "never played"));
  g_clear_error (&error);

  /* A profile without a skin is a profile: it simply has no avatar. */
  {
    g_autoptr (nuvashalauncherMinecraftProfile) none =
      nuvashalauncher_auth_profile_from_json (bare, -1, &error);

    g_assert_no_error (error);
    g_assert_nonnull (none);
    g_assert_cmpstr (none->name, ==, "Notch");
    g_assert_null (none->avatar_url);
  }

  g_print ("ok - the profile says who the player is and what they wear\n");
}

/* Exercise the real token-step transition, rather than only its JSON parser.
 * An expired device code must complete the asynchronous sign-in with a
 * declined error and release the request state; it must not leave a polling
 * source behind. */
static void
test_expired_token_completes_sign_in (void)
{
  g_autofree char *body = fixture ("auth-token-expired.json");
  AuthWait wait = { 0 };
  AuthRequest *request = NULL;

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.scope = nuvashalauncher_task_scope_new (NULL, NULL,
                                               auth_error_finished, &wait);
  request = request_new (wait.scope);
  request->status = 400;
  request->body = g_strdup (body);
  request->deadline = g_get_real_time () / 1000000 + 60;

  token_answered (request);
  g_main_loop_run (wait.loop);

  g_assert_null (wait.scope);
  g_assert_error (wait.error, nuvashalauncher_AUTH_ERROR,
                  nuvashalauncher_AUTH_ERROR_DECLINED);
  g_assert_nonnull (strstr (wait.error->message, "code ran out"));
  g_clear_error (&wait.error);
  g_main_loop_unref (wait.loop);

  g_print ("ok - an expired token response completes sign-in as declined\n");
}

static void
test_service_state_refusals_complete_sign_in (void)
{
  /* These are the actual answer handlers, not just parser calls: every
   * refusal must finish the same asynchronous request exactly once. */
  run_auth_error_answer ("auth-xsts-child-account.json", 401, xsts_answered,
                         nuvashalauncher_AUTH_ERROR_XBOX, "child account");
  run_auth_error_answer ("auth-minecraft-app-not-registered.json", 403,
                         minecraft_answered, nuvashalauncher_AUTH_ERROR_XBOX,
                         "Invalid app registration");
  run_auth_error_answer ("auth-entitlements-not-owned.json", 200,
                         entitlements_answered, nuvashalauncher_AUTH_ERROR_NO_GAME,
                         "Minecraft: Java");
  run_auth_error_answer ("auth-profile-not-found.json", 404, profile_answered,
                         nuvashalauncher_AUTH_ERROR_NO_PROFILE, "never played");

  g_print ("ok - service refusals complete each authentication state\n");
}

static void
test_successful_sign_in_refresh_and_persistence (void)
{
  nuvashalauncherDeviceCode code = { 0 };
  AuthSuccessWait wait = { 0 };
  g_autoptr (nuvashalauncherAccount) account = NULL;
  g_autoptr (nuvashalauncherAccounts) accounts = NULL;
  g_autoptr (nuvashalauncherAccounts) reopened = NULL;
  g_autofree char *directory = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;

  code.device_code = (char *) "device-code-for-test";
  code.interval = 1;
  code.expires_in = 600;
  wait.loop = g_main_loop_new (NULL, FALSE);
  nuvashalauncher_auth_set_transport_for_tests (auth_fixture_transport, NULL);

  wait.scope = nuvashalauncher_auth_sign_in_async (&code, auth_success_finished, &wait);
  g_main_loop_run (wait.loop);
  g_assert_null (wait.scope);
  g_assert_no_error (wait.error);
  g_assert_nonnull (wait.account);
  account = g_steal_pointer (&wait.account);
  g_assert_cmpint (account->type, ==, nuvashalauncher_ACCOUNT_MICROSOFT);
  g_assert_cmpstr (account->name, ==, "Notch");
  g_assert_cmpstr (account->uuid, ==, "069a79f4-44e9-4726-a5be-fca90e38aaf5");
  g_assert_cmpstr (account->access_token, ==,
                   "eyJhbGciOiJIUzI1NiJ9.minecraft.services.token");
  g_assert_cmpstr (account->refresh_token, ==, "M.R3_BAY.the-refresh-token");

  /* The refresh path starts with the stored Microsoft refresh token and the
   * same production service chain replaces the access token/profile state. */
  wait.refresh = TRUE;
  wait.scope = nuvashalauncher_auth_refresh_async (account, auth_success_finished, &wait);
  g_main_loop_run (wait.loop);
  g_assert_null (wait.scope);
  g_assert_no_error (wait.error);
  g_assert_nonnull (wait.account);
  g_assert_cmpstr (wait.account->refresh_token, ==, account->refresh_token);
  g_assert_cmpstr (wait.account->uuid, ==, account->uuid);
  g_clear_pointer (&wait.account, nuvashalauncher_account_free);

  directory = g_dir_make_tmp ("nuvashalauncher-auth-XXXXXX", &error);
  g_assert_no_error (error);
  path = g_build_filename (directory, "accounts.json", NULL);
  accounts = nuvashalauncher_accounts_new_for_path (path);
  if (!nuvashalauncher_accounts_add (accounts, account, &error))
    {
      g_test_skip ("Secret Service is unavailable in this environment");
      nuvashalauncher_auth_set_transport_for_tests (NULL, NULL);
      g_clear_error (&wait.error);
      g_main_loop_unref (wait.loop);
      return;
    }
  g_assert_no_error (error);
  reopened = nuvashalauncher_accounts_new_for_path (path);
  g_assert_cmpuint (nuvashalauncher_accounts_count (reopened), ==, 1);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active (reopened)->access_token, ==,
                   account->access_token);
  g_assert_cmpstr (nuvashalauncher_accounts_get_active (reopened)->refresh_token, ==,
                   account->refresh_token);

  nuvashalauncher_auth_set_transport_for_tests (NULL, NULL);
  g_clear_error (&wait.error);
  g_main_loop_unref (wait.loop);
  g_assert_true (nuvashalauncher_accounts_remove (
    accounts, account->id, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_rmdir (directory), ==, 0);

  g_print ("ok - successful sign-in, refresh and persistence preserve identity\n");
}

/* ---------------------------------------------------------- *
 * The client id
 * ---------------------------------------------------------- */

static void
test_the_endpoints_are_the_documented_ones (void)
{
  g_assert_cmpstr (nuvashalauncher_MICROSOFT_TENANT, ==, "consumers");
  g_assert_cmpstr (nuvashalauncher_MICROSOFT_SCOPE, ==, "XboxLive.signin offline_access");
  g_assert_cmpstr (nuvashalauncher_MINECRAFT_RELYING_PARTY, ==,
                   "rp://api.minecraftservices.com/");
  g_assert_cmpstr (nuvashalauncher_XBOX_CONTRACT_VERSION, ==, "1");

  g_assert_cmpstr (nuvashalauncher_MICROSOFT_DEVICE_CODE_URL, ==,
                   "https://login.microsoftonline.com/consumers/oauth2/v2.0/"
                   "devicecode");
  g_assert_cmpstr (nuvashalauncher_MICROSOFT_TOKEN_URL, ==,
                   "https://login.microsoftonline.com/consumers/oauth2/v2.0/"
                   "token");
  g_assert_cmpstr (nuvashalauncher_XBOX_USER_URL, ==,
                   "https://user.auth.xboxlive.com/user/authenticate");
  g_assert_cmpstr (nuvashalauncher_XBOX_XSTS_URL, ==,
                   "https://xsts.auth.xboxlive.com/xsts/authorize");

  /* The token is never a password and never a secret of the build: what
     is configured is the application id, which Microsoft publishes for
     every application it has. */
  g_assert_cmpuint (strlen (nuvashalauncher_MICROSOFT_CLIENT_ID), >, 0);

  g_print ("ok - the chain asks the addresses the services document\n");
}

int
main (void)
{
  g_print ("# Microsoft sign-in tests\n");

  test_device_code ();
  test_device_code_errors ();
  test_polling ();
  test_token ();
  test_xbox_token ();
  test_xsts_refusals ();
  test_minecraft_token ();
  test_entitlements ();
  test_profile ();
  test_expired_token_completes_sign_in ();
  test_service_state_refusals_complete_sign_in ();
  test_successful_sign_in_refresh_and_persistence ();
  test_the_endpoints_are_the_documented_ones ();

  return EXIT_SUCCESS;
}
