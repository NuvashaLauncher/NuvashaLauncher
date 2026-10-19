#ifndef nuvasha_AUTH_MICROSOFT_PRIVATE_H
#define nuvasha_AUTH_MICROSOFT_PRIVATE_H

#include "nuvasha-auth-microsoft.h"
#include <libsoup/soup.h>

#define MAX_AUTH_RESPONSE_SIZE (4ULL * 1024ULL * 1024ULL)

typedef enum {
  STEP_DEVICE_CODE, STEP_TOKEN, STEP_XBL, STEP_XSTS, STEP_MINECRAFT,
  STEP_ENTITLEMENTS, STEP_PROFILE
} Step;

typedef struct _AuthRequest {
  GTask *task; SoupMessage *message; guint poll_source; NuvashaTaskScope *scope; Step step;
  gint status; char *body; GInputStream *response_stream; GByteArray *response_bytes;
  char *device_code; char *refresh_token; guint interval; gint64 deadline; gint64 now;
  char *microsoft_refresh_token; char *xbl_token; char *xbl_user_hash;
  char *xsts_token; char *mc_access_token; gint64 mc_expires_at;
} AuthRequest;

void nuvasha_auth_request_free (gpointer data);
void sent (SoupSession *session, GAsyncResult *result, gpointer user_data);
void auth_read_ready (GObject *source, GAsyncResult *result, gpointer user_data);

#endif

gboolean client_id_is_set (void);
GError *no_client_id_error (void);
void fail (AuthRequest *self, GError *error);
void succeed (AuthRequest *self, gpointer result, GDestroyNotify free_result);
AuthRequest *request_new (NuvashaTaskScope *scope);
void nuvasha_auth_send (AuthRequest *self, const char *url, const char *method, const char *body, const char *content_type, const char **headers, Step step);
GError *refusal (const char *body, gint status, NuvashaAuthError kind, const char *fallback);
void device_code_answered (AuthRequest *self);

gboolean answer_if_cancelled (AuthRequest *self);
void token_answered (AuthRequest *self);
void ask_xbl (AuthRequest *self, const char *access_token);
void ask_for_token (AuthRequest *self);
void xbl_answered (AuthRequest *self);
void xsts_answered (AuthRequest *self);
void minecraft_answered (AuthRequest *self);
void entitlements_answered (AuthRequest *self);
void profile_answered (AuthRequest *self);
void ask_xsts (AuthRequest *self);
void ask_minecraft (AuthRequest *self);
void ask_entitlements (AuthRequest *self);
void ask_profile (AuthRequest *self);
