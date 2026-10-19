/* Microsoft xbox authentication stage. */

#include "nuvasha-auth-microsoft-private.h"

void
ask_xbl (AuthRequest *self, const char *access_token)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *body = NULL;
  const char *headers[] = { "x-xbl-contract-version",
                            nuvasha_XBOX_CONTRACT_VERSION, "Accept",
                            "application/json", NULL };

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "Properties");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "AuthMethod");
  json_builder_add_string_value (builder, "RPS");
  json_builder_set_member_name (builder, "SiteName");
  json_builder_add_string_value (builder, "user.auth.xboxlive.com");
  g_autofree char *rps_ticket = g_strdup_printf ("d=%s", access_token);

  json_builder_set_member_name (builder, "RpsTicket");
  json_builder_add_string_value (builder, rps_ticket);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "RelyingParty");
  json_builder_add_string_value (builder, "http://auth.xboxlive.com");
  json_builder_set_member_name (builder, "TokenType");
  json_builder_add_string_value (builder, "JWT");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  body = json_generator_to_data (generator, NULL);
  nuvasha_auth_send (self, nuvasha_XBOX_USER_URL, SOUP_METHOD_POST, body,
                     "application/json", headers, STEP_XBL);
}

void
xbl_answered (AuthRequest *self)
{
  g_autoptr (NuvashaXboxToken) token = NULL;
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
    {
      fail (self, refusal (self->body, self->status, nuvasha_AUTH_ERROR_XBOX,
                           "Xbox Live refused the sign-in"));
      return;
    }

  token = nuvasha_auth_xbox_token_from_json (self->body, -1, &error);

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
