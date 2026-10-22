/* Microsoft device-code authentication step. */

#include "nuvasha-auth-microsoft-private.h"
#include "nuvasha-i18n.h"

void
device_code_answered (AuthRequest *self)
{
  g_autoptr (NuvashaDeviceCode) code = NULL;
  g_autoptr (GError) error = NULL;

  if (self->status < 200 || self->status > 299)
    {
      fail (self,
            refusal (self->body, self->status, nuvasha_AUTH_ERROR_DEVICE_CODE,
                     _("Microsoft would not give a code to sign in with")));
      return;
    }

  code = nuvasha_auth_device_code_from_json (self->body, -1, &error);

  if (code == NULL)
    fail (self, g_steal_pointer (&error));
  else
    succeed (self, g_steal_pointer (&code),
             (GDestroyNotify) nuvasha_device_code_free);
}

NuvashaTaskScope *
nuvasha_auth_device_code_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  g_autofree char *form = NULL;
  AuthRequest *self = NULL;

  self = request_new (scope);

  if (!client_id_is_set ())
    {
      fail (self, no_client_id_error ());
      return g_steal_pointer (&scope);
    }

  form = soup_form_encode ("client_id", nuvasha_MICROSOFT_CLIENT_ID, "scope",
                           nuvasha_MICROSOFT_SCOPE, NULL);

  nuvasha_auth_send (self, nuvasha_MICROSOFT_DEVICE_CODE_URL, SOUP_METHOD_POST, form,
        "application/x-www-form-urlencoded", NULL, STEP_DEVICE_CODE);

  return g_steal_pointer (&scope);
}

NuvashaDeviceCode *
nuvasha_auth_device_code_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return (NuvashaDeviceCode *) g_task_propagate_pointer (G_TASK (result), error);
}
