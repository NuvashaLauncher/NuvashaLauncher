/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaAuthMicrosoft: signing in with a Microsoft account and trading the
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
 * Microsoft serves, in a browser of their own, and nuvasha only ever sees the
 * tokens that come back. Those tokens are handed to the caller and to
 * nobody else: they are never logged, never put in an error and never
 * written outside the accounts file, which is 0600.
 *
 * A build must define nuvasha_MICROSOFT_CLIENT_ID as the application (client)
 * ID of an Azure application
 * registered for the device code flow of a public client. When it is
 * empty, signing in fails here with nuvasha_AUTH_ERROR_NO_CLIENT_ID and says
 * which constant to fill in, rather than asking a user for a sign-in
 * this build cannot finish. */

#ifndef nuvasha_AUTH_MICROSOFT_H
#define nuvasha_AUTH_MICROSOFT_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "nuvasha-accounts.h"
#include "nuvasha-task.h"

G_BEGIN_DECLS

/* The application (client) ID of the Azure application nuvasha signs in
 * with. It has to be registered for the device code flow of a public
 * client, and it is the only place a sign-in is configured: an empty
 * constant is what a build without one says out loud. */
#define nuvasha_MICROSOFT_CLIENT_ID "fe820571-8833-4058-b29a-e4d2c28f6beb"

/* Tenant used for personal Microsoft accounts. */
#define nuvasha_MICROSOFT_TENANT "consumers"

/* Permissions for Xbox Live sign-in and refresh tokens. */
#define nuvasha_MICROSOFT_SCOPE "XboxLive.signin offline_access"

#define nuvasha_MICROSOFT_DEVICE_CODE_URL \
  "https://login.microsoftonline.com/" nuvasha_MICROSOFT_TENANT "/oauth2/v2.0/devicecode"
#define nuvasha_MICROSOFT_TOKEN_URL \
  "https://login.microsoftonline.com/" nuvasha_MICROSOFT_TENANT "/oauth2/v2.0/token"
#define nuvasha_XBOX_USER_URL "https://user.auth.xboxlive.com/user/authenticate"
#define nuvasha_XBOX_XSTS_URL "https://xsts.auth.xboxlive.com/xsts/authorize"
#define nuvasha_MINECRAFT_SERVICES_URL "https://api.minecraftservices.com"

/* User-Agent sent to Microsoft services. */
#define nuvasha_AUTH_USER_AGENT "nuvasha"

/* The contract version the Xbox services answer the version they are
 * asked for with. */
#define nuvasha_XBOX_CONTRACT_VERSION "1"

/* The relying party of the X token the Minecraft services accept. */
#define nuvasha_MINECRAFT_RELYING_PARTY "rp://api.minecraftservices.com/"

/* How long a service is given to answer before the chain gives up on it,
 * so that a request that never comes back cannot hang a sign-in. */
#define nuvasha_AUTH_TIMEOUT_SECONDS 30

#define nuvasha_AUTH_ERROR (nuvasha_auth_error_quark ())

typedef enum {
  nuvasha_AUTH_ERROR_NO_CLIENT_ID,  /* the constant is empty */
  nuvasha_AUTH_ERROR_DEVICE_CODE,   /* Microsoft would not give a code */
  nuvasha_AUTH_ERROR_DECLINED,      /* the user said no, or the code ran out */
  nuvasha_AUTH_ERROR_PENDING,       /* the user has not signed in yet */
  nuvasha_AUTH_ERROR_XBOX,          /* Xbox Live or XSTS refused */
  nuvasha_AUTH_ERROR_NO_GAME,       /* the account does not own Minecraft: Java */
  nuvasha_AUTH_ERROR_NO_PROFILE,    /* it owns it but has never played */
  nuvasha_AUTH_ERROR_NETWORK,       /* the request never arrived */
  nuvasha_AUTH_ERROR_PARSE          /* an answer nuvasha cannot read */
} NuvashaAuthError;

GQuark nuvasha_auth_error_quark (void);

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
} NuvashaDeviceCode;

void nuvasha_device_code_free (NuvashaDeviceCode *code);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaDeviceCode, nuvasha_device_code_free)

/* Asks Microsoft for a code to sign in with.
 *
 * nuvasha_AUTH_ERROR_NO_CLIENT_ID when nuvasha_MICROSOFT_CLIENT_ID is empty. */
NuvashaTaskScope *nuvasha_auth_device_code_async (GAsyncReadyCallback callback,
                                          gpointer user_data);

/* (transfer full) */
NuvashaDeviceCode *nuvasha_auth_device_code_finish (GAsyncResult *result,
                                            GError      **error);

/* Poll the token endpoint until sign-in completes, then retrieve the
 * Minecraft account and profile.
 *
 * Cancellable: cancelling stops the polling and nothing is stored.
 *
 * nuvasha_AUTH_ERROR_DECLINED when the user said no or the code ran out,
 * nuvasha_AUTH_ERROR_XBOX when the Microsoft account has no Xbox profile or
 * is a child account, nuvasha_AUTH_ERROR_NO_GAME when it does not own
 * Minecraft: Java, and nuvasha_AUTH_ERROR_NO_PROFILE when it owns it but
 * has never played. */
NuvashaTaskScope *nuvasha_auth_sign_in_async (const NuvashaDeviceCode *code,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/* (transfer full). The caller owns and stores the returned account. */
NuvashaAccount *nuvasha_auth_sign_in_finish (GAsyncResult *result, GError **error);

/* Refresh an account's access token and retrieve its current profile.
 *
 * nuvasha_AUTH_ERROR_DECLINED when Microsoft requires sign-in again. */
NuvashaTaskScope *nuvasha_auth_refresh_async (const NuvashaAccount *account,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

/* (transfer full) */
NuvashaAccount *nuvasha_auth_refresh_finish (GAsyncResult *result, GError **error);

/* ---------------------------------------------------------- *
 * One answer of the chain on its own
 *
 * Each service response is parsed independently. Tests provide fixtures to
 * these functions, which are also used by the live authentication chain.
 * ---------------------------------------------------------- */

/* The device authorization answer. (transfer full) */
NuvashaDeviceCode *nuvasha_auth_device_code_from_json (const char *json,
                                               gssize      length,
                                               GError    **error);

/* The token answer, with the moment the token stops being good already
 * turned into a moment in time. */
typedef struct {
  char *access_token;
  char *refresh_token;
  gint64 expires_at;
} NuvashaOAuthToken;

void nuvasha_oauth_token_free (NuvashaOAuthToken *token);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaOAuthToken, nuvasha_oauth_token_free)

/* (transfer full) */
NuvashaOAuthToken *nuvasha_auth_oauth_token_from_json (const char  *json,
                                               gssize       length,
                                               gint64       now,
                                               GError     **error);

/* The refusals the token endpoint answers the polling with, in the words
 * of what they mean for a user: @error is NULL while the user has simply
 * not signed in yet. */
gboolean nuvasha_auth_polling_error (const char *json, gssize length,
                                 gboolean *slow_down, GError **error);

/* What Xbox Live and XSTS answer with: a token and the hash of the user
 * it is for. */
typedef struct {
  char *token;
  char *user_hash;
} NuvashaXboxToken;

void nuvasha_xbox_token_free (NuvashaXboxToken *token);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaXboxToken, nuvasha_xbox_token_free)

/* (transfer full). The refusals of XSTS come as a number, and the known
   ones are given the words a user can do something with: no Xbox
   profile behind the Microsoft account, a child account that has to be
   added to a family, an account from a country the services do not
   serve. */
NuvashaXboxToken *nuvasha_auth_xbox_token_from_json (const char *json,
                                             gssize      length,
                                             GError    **error);

/* The identity token the services are given: "XBL3.0 x=<hash>;<token>".
 * (transfer full) */
char *nuvasha_auth_identity_token (const char *user_hash, const char *xsts_token);

/* What the services answer a sign-in with: the token the game is run
 * with and the moment it stops being good. */
NuvashaOAuthToken *nuvasha_auth_minecraft_token_from_json (const char  *json,
                                                   gssize       length,
                                                   gint64       now,
                                                   GError     **error);

/* Whether the account owns Minecraft: Java. The services answer with
 * everything the account owns, and an account that owns nothing gets an
 * empty list rather than an error, so that nuvasha can say what is missing
 * instead of repeating that something failed.
 * nuvasha_AUTH_ERROR_NO_GAME when it does not own it. */
gboolean nuvasha_auth_owns_minecraft_from_json (const char *json, gssize length,
                                            GError    **error);

/* The profile: who the player is and what they look like. */
typedef struct {
  char *id;          /* 32 hex characters, as the services publish it */
  char *name;
  char *avatar_url;  /* the skin that is worn, NULL when there is none */
  char *skin_model;  /* CLASSIC or SLIM */
  char *cape_id;     /* currently selected cape, or NULL */
} NuvashaMinecraftProfile;

void nuvasha_minecraft_profile_free (NuvashaMinecraftProfile *profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaMinecraftProfile, nuvasha_minecraft_profile_free)

/* (transfer full). nuvasha_AUTH_ERROR_NO_PROFILE when the services answer
 * 404, which is what an account that owns the game and has never played
 * it says. */
NuvashaMinecraftProfile *nuvasha_auth_profile_from_json (const char *json,
                                                 gssize      length,
                                                 GError    **error);

/* The uuid of a profile, dashed and lowercase as the game is given it.
 * (transfer full) */
char *nuvasha_auth_profile_uuid (const char *id);

G_END_DECLS

#endif /* nuvasha_AUTH_MICROSOFT_H */
