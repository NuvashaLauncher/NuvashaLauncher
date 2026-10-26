/* Microsoft device-code authentication step. */

#include "nuvashalauncher-auth-microsoft-private.h"
#include "nuvashalauncher-i18n.h"

void
device_code_answered (AuthRequest *self)
{
  g_autoptr (nuvashalauncherDeviceCode) code = NULL;
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
    {
      fail (self,
            refusal (self->body, self->status, nuvashalauncher_AUTH_ERROR_DEVICE_CODE,
                     _("Microsoft would not give a code to sign in with")));
      return;
    }

  code = nuvashalauncher_auth_device_code_from_json (self->body, -1, &error);

  if (code == NULL)
    fail (self, g_steal_pointer (&error));
  else
    succeed (self, g_steal_pointer (&code),
             (GDestroyNotify) nuvashalauncher_device_code_free);
}

nuvashalauncherTaskScope *
nuvashalauncher_auth_device_code_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autofree char *form = NULL;
  AuthRequest *self = NULL;

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  form = soup_form_encode ("client_id", nuvashalauncher_MICROSOFT_CLIENT_ID, "scope",
                           nuvashalauncher_MICROSOFT_SCOPE, NULL);

  nuvashalauncher_auth_send (self, nuvashalauncher_MICROSOFT_DEVICE_CODE_URL, SOUP_METHOD_POST, form,
        "application/x-www-form-urlencoded", NULL, STEP_DEVICE_CODE);

  return g_steal_pointer (&scope);
}

nuvashalauncherDeviceCode *
nuvashalauncher_auth_device_code_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (nuvashalauncherDeviceCode *) g_task_propagate_pointer (G_TASK (result), error);
}
