/* Microsoft minecraft authentication stage. */

#include "nuvashalauncher-auth-microsoft-private.h"

void
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

  nuvashalauncher_auth_send (self, nuvashalauncher_MINECRAFT_SERVICES_URL "/launcher/login",
        SOUP_METHOD_POST, body, "application/json", headers, STEP_MINECRAFT);
}

void
minecraft_answered (AuthRequest *self)
{
  g_autoptr (nuvashalauncherOAuthToken) token = NULL;
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
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
