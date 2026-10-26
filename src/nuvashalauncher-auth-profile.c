/* Microsoft profile authentication stage. */

#include "nuvashalauncher-auth-microsoft-private.h"

void
ask_profile (AuthRequest *self)
{
  g_autofree char *authorization = NULL;
  const char *headers[] = { "Authorization", NULL, "Accept",
                            "application/json", NULL };

  authorization = g_strconcat ("Bearer ", self->mc_access_token, NULL);
  headers[1] = authorization;

  nuvashalauncher_auth_send (self, nuvashalauncher_MINECRAFT_SERVICES_URL "/minecraft/profile",
        SOUP_METHOD_GET, NULL, NULL, headers, STEP_PROFILE);
}

void
profile_answered (AuthRequest *self)
{
  g_autoptr (nuvashalauncherMinecraftProfile) profile = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = NULL;
  nuvashalauncherAccount *account = NULL;

  profile = nuvashalauncher_auth_profile_from_json (self->body, -1, &error);

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
      fail (self, refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_NO_PROFILE,
                           "the Minecraft services have no profile for this "
                           "account"));
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
  account = g_new0 (nuvashalauncherAccount, 1);
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
