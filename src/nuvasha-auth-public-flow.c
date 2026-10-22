/* Microsoft sign-in and refresh public flow. */

#include "nuvasha-auth-microsoft-private.h"

NuvashaTaskScope *
nuvasha_auth_sign_in_async (const NuvashaDeviceCode   *code,
                        GAsyncReadyCallback    callback,
                        gpointer               user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  AuthRequest *self = NULL;

  nuvasha_task_scope_set_source_tag (scope, "nuvasha_auth_sign_in_async");

  g_return_val_if_fail (code != NULL, NULL);
  g_return_val_if_fail (code->device_code != NULL, NULL);

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  self->device_code = g_strdup (code->device_code);
  self->interval = code->interval > 0 ? code->interval : 5;
  self->deadline = self->now + code->expires_in;

  ask_for_token (self);

  return g_steal_pointer (&scope);
}

NuvashaAccount *
nuvasha_auth_sign_in_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result,
                                                       "nuvasha_auth_sign_in_async"), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (NuvashaAccount *) g_task_propagate_pointer (G_TASK (result), error);
}

NuvashaTaskScope *
nuvasha_auth_refresh_async (const NuvashaAccount   *account,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  AuthRequest *self = NULL;

  nuvasha_task_scope_set_source_tag (scope, "nuvasha_auth_refresh_async");

  g_return_val_if_fail (account != NULL, NULL);

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  /* An account with no refresh token has nothing to be refreshed with,
     and the only thing to be done about it is for the user to sign in
     again: what the caller is told. */
  if (account->refresh_token == NULL || *account->refresh_token == '\0')
    {
      fail (self,
            g_error_new (nuvasha_AUTH_ERROR, nuvasha_AUTH_ERROR_DECLINED,
                         "\"%s\" has to sign in again before it can play",
                         account->name != NULL ? account->name
                                               : "this account"));
      return g_steal_pointer (&scope);
    }

  self->refresh_token = g_strdup (account->refresh_token);
  /* Microsoft does not always return a new refresh token on a refresh;
     keep the one the account already holds when it does not. */
  self->microsoft_refresh_token = g_strdup (account->refresh_token);

  ask_for_token (self);

  return g_steal_pointer (&scope);
}

NuvashaAccount *
nuvasha_auth_refresh_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result,
                                                       "nuvasha_auth_refresh_async"), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (NuvashaAccount *) g_task_propagate_pointer (G_TASK (result), error);
}
