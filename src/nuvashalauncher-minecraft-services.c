/* Minecraft Services HTTP policy shared by profile, skin and cape requests. */
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-i18n.h"

#include <errno.h>
#include <stdlib.h>

#define MINECRAFT_SERVICES_USER_AGENT "nuvashalauncher/" nuvashalauncher_VERSION

GQuark
nuvashalauncher_minecraft_services_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-minecraft-services-error");
}

SoupSession *
nuvashalauncher_minecraft_services_session (void)
{
  static SoupSession *session;

  if (session == NULL)
    session = soup_session_new_with_options ("user-agent",
                                             MINECRAFT_SERVICES_USER_AGENT,
                                             "timeout", 30u,
                                             NULL);
  return g_object_ref (session);
}

SoupMessage *
nuvashalauncher_minecraft_services_message (const char *method,
                                     const char *path,
                                     const char *access_token)
{
  SoupMessage *message;
  g_autofree char *authorization = NULL;
  g_autofree char *url = NULL;

  g_return_val_if_fail (method != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  url = g_strconcat ("https://api.minecraftservices.com", path, NULL);
  message = soup_message_new (method, url);
  if (message == NULL)
    return NULL;

  soup_message_headers_replace (soup_message_get_request_headers (message),
                                "Accept", "application/json");
  if (access_token != NULL && *access_token != '\0')
    {
      authorization = g_strconcat ("Bearer ", access_token, NULL);
      soup_message_headers_replace (soup_message_get_request_headers (message),
                                    "Authorization", authorization);
    }
  return message;
}

guint
nuvashalauncher_minecraft_services_retry_delay (SoupMessage *message, guint attempt)
{
  const char *retry_after;
  char *end = NULL;
  guint64 seconds;

  g_return_val_if_fail (message == NULL || SOUP_IS_MESSAGE (message), 1);

  retry_after = message != NULL
    ? soup_message_headers_get_one (soup_message_get_response_headers (message),
                                    "Retry-After") : NULL;
  if (retry_after != NULL && *retry_after != '\0')
    {
      errno = 0;
      seconds = g_ascii_strtoull (retry_after, &end, 10);
      if (errno == 0 && end != retry_after && *end == '\0')
        return (guint) MIN (seconds, 300);
    }

  return (guint) MIN (1u << MIN (attempt, 7), 120u);
}

GError *
nuvashalauncher_minecraft_services_response_error (SoupMessage *message, GBytes *body)
{
  guint status = message != NULL ? soup_message_get_status (message) : 0;
  gsize length = 0;
  const char *data = body != NULL ? g_bytes_get_data (body, &length) : NULL;
  g_autofree char *detail = data != NULL ? g_strndup (data, length) : NULL;

  /* Edge proxies often return an HTML block page. Do not put that markup in
   * a localized GTK error dialog; retain only API-style text responses. */
  if (detail != NULL && g_str_has_prefix (g_strstrip (detail), "<"))
    g_clear_pointer (&detail, g_free);

  if (status == 429)
    return g_error_new (nuvashalauncher_MINECRAFT_SERVICES_ERROR,
                        nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED,
                        "rate limited");
  if (status < 200 || status >= 300)
    return g_error_new (nuvashalauncher_MINECRAFT_SERVICES_ERROR,
                        nuvashalauncher_MINECRAFT_SERVICES_ERROR_HTTP,
                        _("Minecraft Services rejected the request (HTTP %u)%s%s"),
                        status, detail != NULL && *detail != '\0' ? ": " : "",
                        detail != NULL ? detail : "");
  return NULL;
}

GBytes *
nuvashalauncher_minecraft_services_skin_multipart (GBytes *skin,
                                           const char *variant,
                                           char **content_type)
{
  GByteArray *payload;
  const char *boundary = "------------------------nuvashalauncherSkin";
  gsize length = 0;
  gconstpointer data;

  if (variant == NULL || *variant == '\0')
    variant = "CLASSIC";
  g_return_val_if_fail (skin != NULL, NULL);
  g_return_val_if_fail (content_type != NULL, NULL);

  data = g_bytes_get_data (skin, &length);
  payload = g_byte_array_new ();
  g_byte_array_append (payload, (const guint8 *) "--", 2);
  g_byte_array_append (payload, (const guint8 *) boundary, strlen (boundary));
  g_byte_array_append (payload, (const guint8 *) "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"skin.png\"\r\nContent-Type: image/png\r\n\r\n", strlen ("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"skin.png\"\r\nContent-Type: image/png\r\n\r\n"));
  g_byte_array_append (payload, data, length);
  g_byte_array_append (payload, (const guint8 *) "\r\n--", 4);
  g_byte_array_append (payload, (const guint8 *) boundary, strlen (boundary));
  g_byte_array_append (payload, (const guint8 *) "\r\nContent-Disposition: form-data; name=\"variant\"\r\n\r\n", strlen ("\r\nContent-Disposition: form-data; name=\"variant\"\r\n\r\n"));
  g_byte_array_append (payload, (const guint8 *) variant, strlen (variant));
  g_byte_array_append (payload, (const guint8 *) "\r\n--", 4);
  g_byte_array_append (payload, (const guint8 *) boundary, strlen (boundary));
  g_byte_array_append (payload, (const guint8 *) "--\r\n", 4);
  *content_type = g_strdup_printf ("multipart/form-data; boundary=%s", boundary);
  return g_byte_array_free_to_bytes (payload);
}

typedef struct {
  SoupSession *session;
  SoupMessage *message;
  nuvashalauncherTaskScope *scope;
} MinecraftSend;

static void
minecraft_send_free (MinecraftSend *send)
{
  g_clear_object (&send->message);
  g_clear_object (&send->session);
  g_free (send);
}

static void
minecraft_send_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  MinecraftSend *send = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (
    SOUP_SESSION (source), result, &error);

  if (bytes == NULL)
    {
      if (error == NULL)
        error = g_error_new_literal (nuvashalauncher_MINECRAFT_SERVICES_ERROR,
                                     nuvashalauncher_MINECRAFT_SERVICES_ERROR_NETWORK,
                                     "Minecraft Services request failed");
      nuvashalauncher_task_scope_return_error (send->scope, g_steal_pointer (&error));
    }
  else
    {
      g_autoptr (GError) response_error =
        nuvashalauncher_minecraft_services_response_error (send->message, bytes);
      if (response_error != NULL)
        nuvashalauncher_task_scope_return_error (send->scope,
                                         g_steal_pointer (&response_error));
      else
        nuvashalauncher_task_scope_return_pointer (send->scope,
                                           g_steal_pointer (&bytes),
                                           (GDestroyNotify) g_bytes_unref);
    }
  nuvashalauncher_task_scope_unref (send->scope);
  minecraft_send_free (send);
}

nuvashalauncherTaskScope *
nuvashalauncher_minecraft_services_send_async (SoupMessage *message,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  MinecraftSend *send;
  g_autoptr (nuvashalauncherTaskScope) scope = NULL;

  g_return_val_if_fail (SOUP_IS_MESSAGE (message), NULL);
  scope = nuvashalauncher_task_scope_new (NULL, cancellable, callback, user_data);
  nuvashalauncher_task_scope_set_source_tag (scope, "nuvashalauncher_minecraft_services_send_async");
  send = g_new0 (MinecraftSend, 1);
  send->message = g_object_ref (message);
  send->session = nuvashalauncher_minecraft_services_session ();
  send->scope = nuvashalauncher_task_scope_ref (scope);
  soup_session_send_and_read_async (send->session, send->message,
                                    G_PRIORITY_DEFAULT,
                                    nuvashalauncher_task_scope_get_cancellable (scope),
                                    minecraft_send_done, send);
  return g_steal_pointer (&scope);
}
