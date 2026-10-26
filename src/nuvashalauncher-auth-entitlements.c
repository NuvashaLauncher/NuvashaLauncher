/* Microsoft entitlements authentication stage. */

#include "nuvashalauncher-auth-microsoft-private.h"

void
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
  nuvashalauncher_auth_send (self, url,
        SOUP_METHOD_GET, NULL, NULL, headers, STEP_ENTITLEMENTS);
}

void
entitlements_answered (AuthRequest *self)
{
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
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
