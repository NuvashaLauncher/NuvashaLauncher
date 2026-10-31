/* Microsoft token polling step. */

#include "nuvashalauncher-auth-microsoft-private.h"

static char *
token_form (AuthRequest *self)
{
  if (self->refresh_token != NULL)
    return soup_form_encode ("client_id", nuvashalauncher_MICROSOFT_CLIENT_ID,
                             "grant_type", "refresh_token", "refresh_token",
                             self->refresh_token, "scope",
                             nuvashalauncher_MICROSOFT_SCOPE, NULL);

  return soup_form_encode ("client_id", nuvashalauncher_MICROSOFT_CLIENT_ID, "grant_type",
                           "urn:ietf:params:oauth:grant-type:device_code",
                           "device_code", self->device_code, "scope",
                           nuvashalauncher_MICROSOFT_SCOPE, NULL);
}

void
ask_for_token (AuthRequest *self)
{
  g_autofree char *form = token_form (self);

  nuvashalauncher_auth_send (self, nuvashalauncher_MICROSOFT_TOKEN_URL, SOUP_METHOD_POST, form,
        "application/x-www-form-urlencoded", NULL, STEP_TOKEN);
}

/* The next poll, once the interval the service asked for has passed. */
static gboolean
poll_again (gpointer user_data)
{
  AuthRequest *self = user_data;
  guint source_id = self->poll_source;

  self->poll_source = 0;
  nuvashalauncher_task_scope_forget_source (self->scope, source_id);

  if (self->task == NULL || answer_if_cancelled (self))
    return G_SOURCE_REMOVE;

  ask_for_token (self);

  return G_SOURCE_REMOVE;
}

void
token_answered (AuthRequest *self)
{
  g_autoptr (nuvashalauncherOAuthToken) token = NULL;
  g_autoptr (GError) error = NULL;
  gboolean slow_down = FALSE;

  if (self->status == 200)
    {
      token = nuvashalauncher_auth_oauth_token_from_json (self->body, -1, self->now, &error);

      if (token == NULL)
        {
          fail (self, g_steal_pointer (&error));
          return;
        }

      /* The refresh token is what the account keeps, so that the sign-in
         outlives the token it starts with. */
      if (token->refresh_token != NULL)
        {
          g_free (self->microsoft_refresh_token);
          self->microsoft_refresh_token = g_strdup (token->refresh_token);
        }

      ask_xbl (self, token->access_token);
      return;
    }

  /* Every poll answers that the user has not signed in yet until they
     have, and that is the wait rather than a failure. */
  if (nuvashalauncher_auth_polling_error (self->body, -1, &slow_down, &error))
    {
      if (error == NULL)
        error = g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_PARSE,
                                     "the service refused the sign-in without "
                                     "saying why");

      fail (self, g_steal_pointer (&error));
      return;
    }

  /* Respect the service's request to slow down polling. */
  if (slow_down)
    self->interval += 5;

  /* The code is good for a quarter of an hour and no longer, so a user
     who walked away is told rather than left waiting. */
  if (self->deadline > 0 &&
      g_get_real_time () / 1000000 >= self->deadline)
    {
      fail (self,
            g_error_new_literal (nuvashalauncher_AUTH_ERROR, nuvashalauncher_AUTH_ERROR_EXPIRED,
                                 "the code ran out before the sign-in was "
                                 "finished: ask for a new one"));
      return;
    }

  self->poll_source = g_timeout_add_seconds (self->interval, poll_again, self);
  g_source_set_name_by_id (self->poll_source, "nuvashalauncher-auth-token-poll");
  nuvashalauncher_task_scope_track_source (self->scope, self->poll_source);
}
