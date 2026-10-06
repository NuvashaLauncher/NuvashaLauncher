/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlAuthMicrosoft: signing in with a Microsoft account and trading the
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
 * Microsoft serves, in a browser of their own, and AML only ever sees the
 * tokens that come back. Those tokens are handed to the caller and to
 * nobody else: they are never logged, never put in an error and never
 * written outside the accounts file, which is 0600.
 *
 * What a build without a registered application says: AML_MICROSOFT_CLIENT_ID
 * has to be the application (client) ID of an Azure application
 * registered for the device code flow of a public client. When it is
 * empty, signing in fails here with AML_AUTH_ERROR_NO_CLIENT_ID and says
 * which constant to fill in, rather than asking a user for a sign-in
 * this build cannot finish. */

#ifndef AML_AUTH_MICROSOFT_H
#define AML_AUTH_MICROSOFT_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "aml-accounts.h"

G_BEGIN_DECLS

/* The application (client) ID of the Azure application AML signs in
 * with. It has to be registered for the device code flow of a public
 * client, and it is the only place a sign-in is configured: an empty
 * constant is what a build without one says out loud. */
#define AML_MICROSOFT_CLIENT_ID "00000000-402b-4cd3-a82b-c45ab2f1d3f7"

/* Personal Microsoft accounts are the ones a Minecraft account is, so the
 * tenant is the one that holds them. */
#define AML_MICROSOFT_TENANT "consumers"

/* What the user consents to: signing in to Xbox Live, and a refresh
 * token so that the sign-in outlives the access token it starts with. */
#define AML_MICROSOFT_SCOPE "XboxLive.signin offline_access"

#define AML_MICROSOFT_DEVICE_CODE_URL \
  "https://login.microsoftonline.com/" AML_MICROSOFT_TENANT "/oauth2/v2.0/devicecode"
#define AML_MICROSOFT_TOKEN_URL \
  "https://login.microsoftonline.com/" AML_MICROSOFT_TENANT "/oauth2/v2.0/token"
#define AML_XBOX_USER_URL "https://user.auth.xboxlive.com/user/authenticate"
#define AML_XBOX_XSTS_URL "https://xsts.auth.xboxlive.com/xsts/authorize"
#define AML_MINECRAFT_SERVICES_URL "https://api.minecraftservices.com"

/* What AML says it is when it asks a service for something. */
#define AML_AUTH_USER_AGENT "AML"

/* The contract version the Xbox services answer the version they are
 * asked for with. */
#define AML_XBOX_CONTRACT_VERSION "1"

/* The relying party of the X token the Minecraft services accept. */
#define AML_MINECRAFT_RELYING_PARTY "rp://api.minecraftservices.com/"

/* How long a service is given to answer before the chain gives up on it,
 * so that a request that never comes back cannot hang a sign-in. */
#define AML_AUTH_TIMEOUT_SECONDS 30

#define AML_AUTH_ERROR (aml_auth_error_quark ())

typedef enum {
  AML_AUTH_ERROR_NO_CLIENT_ID,  /* the constant is empty */
  AML_AUTH_ERROR_DEVICE_CODE,   /* Microsoft would not give a code */
  AML_AUTH_ERROR_DECLINED,      /* the user said no, or the code ran out */
  AML_AUTH_ERROR_PENDING,       /* the user has not signed in yet */
  AML_AUTH_ERROR_XBOX,          /* Xbox Live or XSTS refused */
  AML_AUTH_ERROR_NO_GAME,       /* the account does not own Minecraft: Java */
  AML_AUTH_ERROR_NO_PROFILE,    /* it owns it but has never played */
  AML_AUTH_ERROR_NETWORK,       /* the request never arrived */
  AML_AUTH_ERROR_PARSE          /* an answer AML cannot read */
} AmlAuthError;

GQuark aml_auth_error_quark (void);

/* The code a user has to type into a browser of their own, and where.
 * @message is what Microsoft says, which is the sentence to show a user
 * when there is one.
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
} AmlDeviceCode;

void aml_device_code_free (AmlDeviceCode *code);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlDeviceCode, aml_device_code_free)

/* Asks Microsoft for a code to sign in with.
 *
 * AML_AUTH_ERROR_NO_CLIENT_ID when AML_MICROSOFT_CLIENT_ID is empty. */
void aml_auth_device_code_async (GCancellable       *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data);

/* (transfer full) */
AmlDeviceCode *aml_auth_device_code_finish (GAsyncResult *result,
                                            GError      **error);

/* Polls the token endpoint until the user has signed in, and then runs
 * the rest of the chain: an account that owns Minecraft: Java, with its
 * profile, is what comes back.
 *
 * Cancellable: cancelling stops the polling and nothing is stored.
 *
 * AML_AUTH_ERROR_DECLINED when the user said no or the code ran out,
 * AML_AUTH_ERROR_XBOX when the Microsoft account has no Xbox profile or
 * is a child account, AML_AUTH_ERROR_NO_GAME when it does not own
 * Minecraft: Java, and AML_AUTH_ERROR_NO_PROFILE when it owns it but
 * has never played, which is what an account of a family that only
 * bought the game says. */
void aml_auth_sign_in_async (const AmlDeviceCode *code,
                             GCancellable          *cancellable,
                             GAsyncReadyCallback    callback,
                             gpointer               user_data);

/* (transfer full). The account is not stored anywhere: the caller adds
   it to the store, which is what writes it. */
AmlAccount *aml_auth_sign_in_finish (GAsyncResult *result, GError **error);

/* A new access token for an account that still has a refresh token, and
 * then the rest of the chain again, so the profile of an account that
 * changed its name is what comes back.
 *
 * AML_AUTH_ERROR_DECLINED when Microsoft will not hand out a new one,
 * which is what the caller answers by asking the user to sign in again. */
void aml_auth_refresh_async (const AmlAccount   *account,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data);

/* (transfer full) */
AmlAccount *aml_auth_refresh_finish (GAsyncResult *result, GError **error);

/* ---------------------------------------------------------- *
 * One answer of the chain on its own
 *
 * The chain is a sequence of requests, and each of them is read here on
 * its own: what a service answered is read from what it answered with,
 * and nothing else. These are the seam a test hands a fixture to, and
 * they are the same code the chain above uses.
 * ---------------------------------------------------------- */

/* The device authorization answer. (transfer full) */
AmlDeviceCode *aml_auth_device_code_from_json (const char *json,
                                               gssize      length,
                                               GError    **error);

/* The token answer, with the moment the token stops being good already
 * turned into a moment in time. */
typedef struct {
  char *access_token;
  char *refresh_token;
  gint64 expires_at;
} AmlOAuthToken;

void aml_oauth_token_free (AmlOAuthToken *token);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlOAuthToken, aml_oauth_token_free)

/* (transfer full) */
AmlOAuthToken *aml_auth_oauth_token_from_json (const char  *json,
                                               gssize       length,
                                               gint64       now,
                                               GError     **error);

/* The refusals the token endpoint answers the polling with, in the words
 * of what they mean for a user: @error is NULL while the user has simply
 * not signed in yet. */
gboolean aml_auth_polling_error (const char *json, gssize length,
                                 gboolean *slow_down, GError **error);

/* What Xbox Live and XSTS answer with: a token and the hash of the user
 * it is for. */
typedef struct {
  char *token;
  char *user_hash;
} AmlXboxToken;

void aml_xbox_token_free (AmlXboxToken *token);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlXboxToken, aml_xbox_token_free)

/* (transfer full). The refusals of XSTS come as a number, and the known
   ones are given the words a user can do something with: no Xbox
   profile behind the Microsoft account, a child account that has to be
   added to a family, an account from a country the services do not
   serve. */
AmlXboxToken *aml_auth_xbox_token_from_json (const char *json,
                                             gssize      length,
                                             GError    **error);

/* The identity token the services are given: "XBL3.0 x=<hash>;<token>".
 * (transfer full) */
char *aml_auth_identity_token (const char *user_hash, const char *xsts_token);

/* What the services answer a sign-in with: the token the game is run
 * with and the moment it stops being good. */
AmlOAuthToken *aml_auth_minecraft_token_from_json (const char  *json,
                                                   gssize       length,
                                                   gint64       now,
                                                   GError     **error);

/* Whether the account owns Minecraft: Java. The services answer with
 * everything the account owns, and an account that owns nothing gets an
 * empty list rather than an error, so that AML can say what is missing
 * instead of repeating that something failed.
 * AML_AUTH_ERROR_NO_GAME when it does not own it. */
gboolean aml_auth_owns_minecraft_from_json (const char *json, gssize length,
                                            GError    **error);

/* The profile: who the player is and what they look like. */
typedef struct {
  char *id;          /* 32 hex characters, as the services publish it */
  char *name;
  char *avatar_url;  /* the skin that is worn, NULL when there is none */
} AmlMinecraftProfile;

void aml_minecraft_profile_free (AmlMinecraftProfile *profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlMinecraftProfile, aml_minecraft_profile_free)

/* (transfer full). AML_AUTH_ERROR_NO_PROFILE when the services answer
 * 404, which is what an account that owns the game and has never played
 * it says. */
AmlMinecraftProfile *aml_auth_profile_from_json (const char *json,
                                                 gssize      length,
                                                 GError    **error);

/* The uuid of a profile, dashed and lowercase as the game is given it.
 * (transfer full) */
char *aml_auth_profile_uuid (const char *id);

G_END_DECLS

#endif /* AML_AUTH_MICROSOFT_H */