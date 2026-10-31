/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherAuthMicrosoft: signing in with a Microsoft account and trading the
 * sign-in for a Minecraft profile.
 *
 * The chain is the one the services document, and every step of it is one
 * request and one answer:
 *
 *   1. the device authorization grant, which answers with a short code
 *      and the address a user types it at (Microsoft identity platform,
 *      /consumers/oauth2/v2.0/devicecode)
 *   2. the token endpoint, polled until the user has signed in
 *   3. Xbox Live, which trades the Microsoft token for a user token
 *      (user.auth.xboxlive.com/user/authenticate)
 *   4. XSTS, which trades the user token for an X token for the relying
 *      party of the Minecraft services (xsts.auth.xboxlive.com/xsts/authorize)
 *   5. the services, which trades the X token for the token the game is
 *      run with (api.minecraftservices.com/authentication/login_with_xbox)
 *   6. the entitlements, which say whether the account owns Minecraft:
 *      Java at all
 *   7. the profile, which says who the player is and what they look like
 *
 * No password is ever asked for: the user types a code into a page that
 * Microsoft serves, in a browser of their own, and nuvashalauncher only ever sees the
 * tokens that come back. Those tokens are handed to the caller and to
 * nobody else: they are never logged, never put in an error and never
 * written outside the accounts file, which is 0600.
 *
 * A build must define nuvashalauncher_MICROSOFT_CLIENT_ID as the application (client)
 * ID of an Azure application
 * registered for the device code flow of a public client. When it is
 * empty, signing in fails here with nuvashalauncher_AUTH_ERROR_NO_CLIENT_ID and says
 * which constant to fill in, rather than asking a user for a sign-in
 * this build cannot finish. */

#ifndef nuvashalauncher_AUTH_MICROSOFT_H
#define nuvashalauncher_AUTH_MICROSOFT_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

/* The application (client) ID of the Azure application nuvashalauncher signs in
 * with. It has to be registered for the device code flow of a public
 * client, and it is the only place a sign-in is configured: an empty
 * constant is what a build without one says out loud. */
#define nuvashalauncher_MICROSOFT_CLIENT_ID "fe820571-8833-4058-b29a-e4d2c28f6beb"

/* Tenant used for personal Microsoft accounts. */
#define nuvashalauncher_MICROSOFT_TENANT "consumers"

/* Permissions for Xbox Live sign-in and refresh tokens. */
#define nuvashalauncher_MICROSOFT_SCOPE "XboxLive.signin offline_access"

#define nuvashalauncher_MICROSOFT_DEVICE_CODE_URL \
  "https://login.microsoftonline.com/" nuvashalauncher_MICROSOFT_TENANT "/oauth2/v2.0/devicecode"
#define nuvashalauncher_MICROSOFT_TOKEN_URL \
  "https://login.microsoftonline.com/" nuvashalauncher_MICROSOFT_TENANT "/oauth2/v2.0/token"
#define nuvashalauncher_XBOX_USER_URL "https://user.auth.xboxlive.com/user/authenticate"
#define nuvashalauncher_XBOX_XSTS_URL "https://xsts.auth.xboxlive.com/xsts/authorize"
#define nuvashalauncher_MINECRAFT_SERVICES_URL "https://api.minecraftservices.com"

#define nuvashalauncher_AUTH_USER_AGENT "nuvashalauncher"

/* The contract version the Xbox services answer the version they are
 * asked for with. */
#define nuvashalauncher_XBOX_CONTRACT_VERSION "1"

/* The relying party of the X token the Minecraft services accept. */
#define nuvashalauncher_MINECRAFT_RELYING_PARTY "rp://api.minecraftservices.com/"

/* How long a service is given to answer before the chain gives up on it,
 * so that a request that never comes back cannot hang a sign-in. */
#define nuvashalauncher_AUTH_TIMEOUT_SECONDS 30

#define nuvashalauncher_AUTH_ERROR (nuvashalauncher_auth_error_quark ())

typedef enum {
  nuvashalauncher_AUTH_ERROR_NO_CLIENT_ID,  /* the constant is empty */
  nuvashalauncher_AUTH_ERROR_DEVICE_CODE,   /* Microsoft would not give a code */
  nuvashalauncher_AUTH_ERROR_DECLINED,      /* the user declined sign-in */
  nuvashalauncher_AUTH_ERROR_PENDING,       /* the user has not signed in yet */
  nuvashalauncher_AUTH_ERROR_XBOX,          /* Xbox Live or XSTS refused */
  nuvashalauncher_AUTH_ERROR_NO_GAME,       /* the account does not own Minecraft: Java */
  nuvashalauncher_AUTH_ERROR_NO_PROFILE,    /* it owns Minecraft but has no profile */
  nuvashalauncher_AUTH_ERROR_NETWORK,       /* the request never arrived */
  nuvashalauncher_AUTH_ERROR_PARSE,         /* an answer nuvashalauncher cannot read */
  nuvashalauncher_AUTH_ERROR_EXPIRED,
  nuvashalauncher_AUTH_ERROR_REAUTH,
  nuvashalauncher_AUTH_ERROR_XBOX_PROFILE,
  nuvashalauncher_AUTH_ERROR_XBOX_FAMILY,
  nuvashalauncher_AUTH_ERROR_XBOX_COUNTRY,
  nuvashalauncher_AUTH_ERROR_XBOX_AGE,
  nuvashalauncher_AUTH_ERROR_SERVICE       /* other service refusals; keep saved credentials */
} nuvashalauncherAuthError;

GQuark nuvashalauncher_auth_error_quark (void);

/* Device code displayed to the user and its verification URI.
 * @message contains Microsoft's display text.
 *
 * @device_code is what the token endpoint is polled with and the user
 * never sees it: it is the other half of the same pair the code they do
 * see belongs to. */
typedef struct {
  char *user_code;
  char *verification_uri;
  char *message;
  guint expires_in;
  guint interval; /* how often the token endpoint is to be asked */
  char *device_code;
} nuvashalauncherDeviceCode;

void nuvashalauncher_device_code_free (nuvashalauncherDeviceCode *code);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherDeviceCode, nuvashalauncher_device_code_free)

/* Asks Microsoft for a code to sign in with.
 *
 * nuvashalauncher_AUTH_ERROR_NO_CLIENT_ID when nuvashalauncher_MICROSOFT_CLIENT_ID is empty. */
nuvashalauncherTaskScope *nuvashalauncher_auth_device_code_async (GAsyncReadyCallback callback,
                                          gpointer user_data);

/* (transfer full) */
nuvashalauncherDeviceCode *nuvashalauncher_auth_device_code_finish (GAsyncResult *result,
                                            GError      **error);

/* Poll the token endpoint until sign-in completes, then retrieve the
 * Minecraft account and profile.
 *
 * Cancellable: cancelling stops the polling and nothing is stored.
 *
 * nuvashalauncher_AUTH_ERROR_DECLINED when the user said no,
 * nuvashalauncher_AUTH_ERROR_EXPIRED when the device code ran out,
 * nuvashalauncher_AUTH_ERROR_XBOX when the Microsoft account has no Xbox profile or
 * is a child account, nuvashalauncher_AUTH_ERROR_NO_GAME when it does not own
 * Minecraft: Java, and nuvashalauncher_AUTH_ERROR_NO_PROFILE when it owns it but
 * has never played. */
nuvashalauncherTaskScope *nuvashalauncher_auth_sign_in_async (const nuvashalauncherDeviceCode *code,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/* (transfer full). The caller owns and stores the returned account. */
nuvashalauncherAccount *nuvashalauncher_auth_sign_in_finish (GAsyncResult *result, GError **error);

/* Refresh an account's access token and retrieve its current profile.
 *
 * nuvashalauncher_AUTH_ERROR_REAUTH when Microsoft requires sign-in again. */
nuvashalauncherTaskScope *nuvashalauncher_auth_refresh_async (const nuvashalauncherAccount *account,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/* (transfer full) */
nuvashalauncherAccount *nuvashalauncher_auth_refresh_finish (GAsyncResult *result, GError **error);

/* ---------------------------------------------------------- *
 * Individual authentication responses
 *
 * Each service response is parsed independently. Tests provide fixtures to
 * these functions, which are also used by the live authentication chain.
 * ---------------------------------------------------------- */

/* The device authorization answer. (transfer full) */
nuvashalauncherDeviceCode *nuvashalauncher_auth_device_code_from_json (const char *json,
                                               gssize      length,
                                               GError    **error);

/* The token answer, with the moment the token stops being good already
 * turned into a moment in time. */
typedef struct {
  char *access_token;
  char *refresh_token;
  gint64 expires_at;
} nuvashalauncherOAuthToken;

void nuvashalauncher_oauth_token_free (nuvashalauncherOAuthToken *token);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherOAuthToken, nuvashalauncher_oauth_token_free)

/* (transfer full) */
nuvashalauncherOAuthToken *nuvashalauncher_auth_oauth_token_from_json (const char  *json,
                                               gssize       length,
                                               gint64       now,
                                               GError     **error);

/* The refusals the token endpoint answers the polling with, in the words
 * of what they mean for a user: @error is NULL while the user has simply
 * not signed in yet. */
gboolean nuvashalauncher_auth_polling_error (const char *json, gssize length,
                                 gboolean *slow_down, GError **error);

/* What Xbox Live and XSTS answer with: a token and the hash of the user
 * it is for. */
typedef struct {
  char *token;
  char *user_hash;
} nuvashalauncherXboxToken;

void nuvashalauncher_xbox_token_free (nuvashalauncherXboxToken *token);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherXboxToken, nuvashalauncher_xbox_token_free)

/* (transfer full). The refusals of XSTS come as a number, and the known
   ones are given the words a user can do something with: no Xbox
   profile behind the Microsoft account, a child account that has to be
   added to a family, an account from a country the services do not
   serve. */
nuvashalauncherXboxToken *nuvashalauncher_auth_xbox_token_from_json (const char *json,
                                             gssize      length,
                                             GError    **error);

/* The identity token the services are given: "XBL3.0 x=<hash>;<token>".
 * (transfer full) */
char *nuvashalauncher_auth_identity_token (const char *user_hash, const char *xsts_token);

/* What the services answer a sign-in with: the token the game is run
 * with and the moment it stops being good. */
nuvashalauncherOAuthToken *nuvashalauncher_auth_minecraft_token_from_json (const char  *json,
                                                   gssize       length,
                                                   gint64       now,
                                                   GError     **error);

/* Whether the account owns Minecraft: Java. The services answer with
 * everything the account owns, and an account that owns nothing gets an
 * empty list rather than an error, so that nuvashalauncher can say what is missing
 * instead of repeating that something failed.
 * nuvashalauncher_AUTH_ERROR_NO_GAME when it does not own it. */
gboolean nuvashalauncher_auth_owns_minecraft_from_json (const char *json, gssize length,
                                            GError    **error);

/* The profile: who the player is and what they look like. */
typedef struct {
  char *id;          /* 32 hex characters, as the services publish it */
  char *name;
  char *avatar_url;  /* the skin that is worn, NULL when there is none */
  char *skin_model;  /* CLASSIC or SLIM */
  char *cape_id;     /* currently selected cape, or NULL */
} nuvashalauncherMinecraftProfile;

void nuvashalauncher_minecraft_profile_free (nuvashalauncherMinecraftProfile *profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherMinecraftProfile, nuvashalauncher_minecraft_profile_free)

/* (transfer full). nuvashalauncher_AUTH_ERROR_NO_PROFILE when the services answer
 * 404, which is what an account that owns the game and has never played
 * it says. */
nuvashalauncherMinecraftProfile *nuvashalauncher_auth_profile_from_json (const char *json,
                                                 gssize      length,
                                                 GError    **error);

/* The uuid of a profile, dashed and lowercase as the game is given it.
 * (transfer full) */
char *nuvashalauncher_auth_profile_uuid (const char *id);

G_END_DECLS

#endif /* nuvashalauncher_AUTH_MICROSOFT_H */
