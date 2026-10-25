/* Microsoft xsts authentication stage. */

#include "nuvasha-auth-microsoft-private.h"

void
ask_xsts (AuthRequest *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *body = NULL;
  const char *headers[] = { "x-xbl-contract-version",
                            nuvasha_XBOX_CONTRACT_VERSION, "Accept",
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
  json_builder_add_string_value (builder, nuvasha_MINECRAFT_RELYING_PARTY);
  json_builder_set_member_name (builder, "TokenType");
  json_builder_add_string_value (builder, "JWT");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  body = json_generator_to_data (generator, NULL);

  nuvasha_auth_send (self, nuvasha_XBOX_XSTS_URL, SOUP_METHOD_POST, body, "application/json",
        headers, STEP_XSTS);
}

void
xsts_answered (AuthRequest *self)
{
  g_autoptr (NuvashaXboxToken) token = NULL;
  g_autoptr (GError) error = NULL;

  token = nuvasha_auth_xbox_token_from_json (self->body, -1, &error);

  if (token == NULL)
    {
      fail (self, g_steal_pointer (&error));
      return;
    }

  if (self->status < 200 || self->status > 299)
    {
      fail (self, refusal (self->body, self->status, nuvasha_AUTH_ERROR_XBOX,
                           "Xbox services refused the sign-in"));
      return;
    }

  g_free (self->xsts_token);
  self->xsts_token = g_steal_pointer (&token->token);

  /* The services are told who the player is by the hash the user token
     came with, and without it there is nobody to sign in as. */
  if (self->xbl_user_hash == NULL)
    {
      fail (self, g_error_new_literal (nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_PARSE,
                                       "Xbox services answered with a token "
                                       "and no user"));
      return;
    }

  ask_minecraft (self);
}
