/* Authentication request cleanup. */

#include "nuvashalauncher-auth-microsoft-private.h"

void
nuvashalauncher_auth_request_free (gpointer data)
{
  AuthRequest *self = data;

  /* The scope owns the task this request is the data of, so reaching here
     is the scope going away: it has already released the sources the poll
     was waiting on. */

  g_clear_object (&self->task);
  g_clear_object (&self->message);
  g_clear_object (&self->response_stream);
  g_clear_pointer (&self->response_bytes, g_byte_array_unref);
  g_free (self->body);
  g_free (self->device_code);
  g_free (self->refresh_token);
  g_free (self->microsoft_refresh_token);
  g_free (self->xbl_token);
  g_free (self->xbl_user_hash);
  g_free (self->xsts_token);
  g_free (self->mc_access_token);
  g_free (self);
}
