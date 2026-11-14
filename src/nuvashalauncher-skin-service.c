/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-skin-service.h"
#include "nuvashalauncher-errors.h"
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-skin-view.h"
#include "nuvashalauncher-task.h"
#include "nuvashalauncher-users-popover.h"

#include <libsoup/soup.h>
#include <string.h>

struct _NuvashaLauncherSkinService
{
  grefcount refs;
  NuvashaLauncherAccount *account;
  NuvashaLauncherSkinServiceCallbacks callbacks;
  gpointer user_data;

  gboolean upload_in_flight;
  gint64 upload_retry_after;
  guint upload_retry_source;
  guint upload_retry_attempt;
  char *pending_upload_url;
  char *pending_upload_variant;
  GCancellable *upload_cancellable;
  char *pending_cape_id;
};

typedef struct
{
  NuvashaLauncherSkinService *service;
  char *url;
  char *variant;
  GCancellable *cancellable;
  SoupMessage *message;
  gboolean background_counted;
} SkinUpload;

static guint skin_background_operations = 0;

static void schedule_pending_skin_upload (NuvashaLauncherSkinService *self, guint delay);
static void start_pending_skin_upload (NuvashaLauncherSkinService *self);
static void send_pending_cape_change (NuvashaLauncherSkinService *self);

gboolean
nuvashalauncher_skin_service_background_work_active (void)
{
  return skin_background_operations > 0;
}

gboolean
nuvashalauncher_users_skin_background_work_active (void)
{
  return nuvashalauncher_skin_service_background_work_active ();
}

static void
notify_error (NuvashaLauncherSkinService *self, const GError *error)
{
  if (self != NULL && !nuvashalauncher_error_is_cancelled (error) &&
      self->callbacks.on_error != NULL)
    self->callbacks.on_error (error, self->user_data);
}

static void
skin_upload_free (SkinUpload *upload)
{
  NuvashaLauncherSkinService *service = upload->service;

  g_free (upload->url);
  g_free (upload->variant);
  g_clear_object (&upload->cancellable);
  g_clear_object (&upload->message);
  g_free (upload);
  nuvashalauncher_skin_service_unref (service);
}

static void
skin_upload_background_finish (SkinUpload *upload)
{
  if (upload != NULL && upload->background_counted)
    {
      upload->background_counted = FALSE;
      if (skin_background_operations > 0)
        skin_background_operations--;
    }
}

NuvashaLauncherSkinService *
nuvashalauncher_skin_service_new (const NuvashaLauncherAccount              *account,
                                  const NuvashaLauncherSkinServiceCallbacks *callbacks,
                                  gpointer                                   user_data)
{
  NuvashaLauncherSkinService *self = g_new0 (NuvashaLauncherSkinService, 1);

  g_ref_count_init (&self->refs);
  self->account = account != NULL ? nuvashalauncher_account_copy (account) : NULL;
  if (callbacks != NULL)
    self->callbacks = *callbacks;
  self->user_data = user_data;

  return self;
}

NuvashaLauncherSkinService *
nuvashalauncher_skin_service_ref (NuvashaLauncherSkinService *self)
{
  if (self != NULL)
    g_ref_count_inc (&self->refs);
  return self;
}

static void
skin_service_free (NuvashaLauncherSkinService *self)
{
  if (self->upload_retry_source != 0)
    g_source_remove (self->upload_retry_source);
  g_clear_object (&self->upload_cancellable);
  g_clear_pointer (&self->pending_upload_url, g_free);
  g_clear_pointer (&self->pending_upload_variant, g_free);
  g_clear_pointer (&self->pending_cape_id, g_free);
  g_clear_pointer (&self->account, nuvashalauncher_account_free);
  g_free (self);
}

void
nuvashalauncher_skin_service_unref (NuvashaLauncherSkinService *self)
{
  if (self != NULL && g_ref_count_dec (&self->refs))
    skin_service_free (self);
}

void
nuvashalauncher_skin_service_detach_ui (NuvashaLauncherSkinService *self)
{
  if (self == NULL)
    return;

  memset (&self->callbacks, 0, sizeof (self->callbacks));
  self->user_data = NULL;
}

static void
on_skin_upload_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  SkinUpload *upload = user_data;
  NuvashaLauncherSkinService *self =
    upload != NULL ? nuvashalauncher_skin_service_ref (upload->service) : NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) response =
    g_task_propagate_pointer (G_TASK (result), &error);
  gboolean rate_limited = error != NULL &&
                          error->domain == nuvashalauncher_MINECRAFT_SERVICES_ERROR &&
                          error->code == nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED;

  if (self != NULL && (response == NULL || error != NULL) && rate_limited)
    {
      guint delay = nuvashalauncher_minecraft_services_retry_delay (
        NULL, self->upload_retry_attempt++);

      /* Keep the failed selection unless the user has already selected a
       * newer one. The next attempt always uses the latest pending URL. */
      if (self->pending_upload_url == NULL && upload != NULL)
        {
          self->pending_upload_url = g_strdup (upload->url);
          self->pending_upload_variant = g_strdup (upload->variant);
        }
      self->upload_retry_after = g_get_real_time () / 1000000 + delay;
      schedule_pending_skin_upload (self, delay);
      notify_error (self, error);
    }
  else if (self != NULL && (response == NULL || error != NULL))
    notify_error (self, error);

  if (self != NULL)
    {
      self->upload_in_flight = FALSE;
      g_clear_object (&self->upload_cancellable);
      if (response != NULL && error == NULL)
        {
          self->upload_retry_after = 0;
          self->upload_retry_attempt = 0;
          if (self->callbacks.on_upload_succeeded != NULL)
            self->callbacks.on_upload_succeeded (self->user_data);
        }
    }
  skin_upload_background_finish (upload);
  if (self != NULL && !rate_limited)
    {
      gboolean succeeded = response != NULL && error == NULL;

      start_pending_skin_upload (self);
      if (succeeded && !self->upload_in_flight)
        send_pending_cape_change (self);
    }

  if (upload != NULL)
    skin_upload_free (upload);
  nuvashalauncher_skin_service_unref (self);
}

static void
send_skin_upload (SkinUpload *upload, GBytes *bytes)
{
  SoupMessage *message = nuvashalauncher_minecraft_services_message (
    SOUP_METHOD_POST, "/minecraft/profile/skins",
    upload->service->account->access_token);
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GBytes) prepared = NULL;
  g_autofree char *content_type = NULL;

  prepared = g_ascii_strcasecmp (upload->variant, "CLASSIC") == 0
    ? nuvashalauncher_skin_prepare_classic_upload (bytes) : g_bytes_ref (bytes);
  body = nuvashalauncher_minecraft_services_skin_multipart (prepared, upload->variant,
                                                            &content_type);
  soup_message_set_request_body_from_bytes (message, content_type, body);
  nuvashalauncher_task_scope_unref_when_done (
    nuvashalauncher_minecraft_services_send_async (message, upload->cancellable,
                                                   on_skin_upload_done, upload));
  g_object_unref (message);
}

static void
on_skin_download_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinUpload *upload = user_data;
  NuvashaLauncherSkinService *self = nuvashalauncher_skin_service_ref (upload->service);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (
    SOUP_SESSION (source), result, &error);

  if (upload->message != NULL &&
      !SOUP_STATUS_IS_SUCCESSFUL (soup_message_get_status (upload->message)))
    {
      g_clear_pointer (&bytes, g_bytes_unref);
      g_clear_error (&error);
      g_set_error (&error, nuvashalauncher_MINECRAFT_SERVICES_ERROR,
                   nuvashalauncher_MINECRAFT_SERVICES_ERROR_HTTP,
                   "skin request returned HTTP status %u",
                   soup_message_get_status (upload->message));
    }
  if (bytes == NULL)
    {
      notify_error (self, error);
      self->upload_in_flight = FALSE;
      g_clear_object (&self->upload_cancellable);
      skin_upload_background_finish (upload);
      skin_upload_free (upload);
      start_pending_skin_upload (self);
      nuvashalauncher_skin_service_unref (self);
      return;
    }
  send_skin_upload (upload, bytes);
  nuvashalauncher_skin_service_unref (self);
}

static void
on_skin_upload_file_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinUpload *upload = user_data;
  NuvashaLauncherSkinService *self = nuvashalauncher_skin_service_ref (upload->service);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  if (bytes == NULL)
    {
      notify_error (self, error);
      self->upload_in_flight = FALSE;
      g_clear_object (&self->upload_cancellable);
      skin_upload_background_finish (upload);
      skin_upload_free (upload);
      start_pending_skin_upload (self);
      nuvashalauncher_skin_service_unref (self);
      return;
    }
  if (self->pending_upload_url != NULL)
    {
      self->upload_in_flight = FALSE;
      g_clear_object (&self->upload_cancellable);
      skin_upload_background_finish (upload);
      skin_upload_free (upload);
      start_pending_skin_upload (self);
      nuvashalauncher_skin_service_unref (self);
      return;
    }
  send_skin_upload (upload, bytes);
  nuvashalauncher_skin_service_unref (self);
}

static void
start_pending_skin_upload (NuvashaLauncherSkinService *self)
{
  g_autoptr (GFile) file = NULL;
  SkinUpload *upload;
  gint64 now = g_get_real_time () / 1000000;

  if (self == NULL || self->pending_upload_url == NULL ||
      self->account == NULL || self->account->access_token == NULL ||
      !nuvashalauncher_account_token_is_valid (self->account, now) ||
      self->upload_in_flight)
    return;

  if (self->upload_retry_after > now)
    {
      guint delay = (guint) (self->upload_retry_after - now);

      if (self->upload_retry_source == 0)
        schedule_pending_skin_upload (self, delay);
      return;
    }

  if (self->upload_retry_source != 0)
    {
      g_source_remove (self->upload_retry_source);
      self->upload_retry_source = 0;
    }

  {
    g_autofree char *url = g_steal_pointer (&self->pending_upload_url);
    g_autofree char *variant = g_steal_pointer (&self->pending_upload_variant);

    if (variant == NULL || *variant == '\0')
      variant = g_strdup ("CLASSIC");

    if (!g_str_has_prefix (url, "file://") &&
        !g_str_has_prefix (url, "http://") &&
        !g_str_has_prefix (url, "https://"))
      return;

    self->upload_in_flight = TRUE;
    if (!g_str_has_prefix (url, "file://"))
      {
        g_autoptr (SoupSession) session = nuvashalauncher_minecraft_services_session ();

        upload = g_new0 (SkinUpload, 1);
        upload->service = nuvashalauncher_skin_service_ref (self);
        upload->url = g_steal_pointer (&url);
        upload->variant = g_steal_pointer (&variant);
        upload->cancellable = g_cancellable_new ();
        upload->background_counted = TRUE;
        skin_background_operations++;
        self->upload_cancellable = g_object_ref (upload->cancellable);
        upload->message = soup_message_new (SOUP_METHOD_GET, upload->url);
        soup_session_send_and_read_async (session, upload->message,
                                          G_PRIORITY_DEFAULT, upload->cancellable,
                                          on_skin_download_done, upload);
        return;
      }

    file = g_file_new_for_uri (url);
    upload = g_new0 (SkinUpload, 1);
    upload->service = nuvashalauncher_skin_service_ref (self);
    upload->url = g_steal_pointer (&url);
    upload->variant = g_steal_pointer (&variant);
    upload->cancellable = g_cancellable_new ();
    upload->background_counted = TRUE;
    skin_background_operations++;
    self->upload_cancellable = g_object_ref (upload->cancellable);
    g_file_load_bytes_async (file, upload->cancellable,
                             on_skin_upload_file_loaded, upload);
  }
}

static gboolean
retry_pending_skin_upload (gpointer user_data)
{
  NuvashaLauncherSkinService *self = user_data;

  self->upload_retry_source = 0;
  self->upload_retry_after = 0;
  start_pending_skin_upload (self);
  return G_SOURCE_REMOVE;
}

static void
schedule_pending_skin_upload (NuvashaLauncherSkinService *self, guint delay)
{
  if (self->upload_retry_source != 0)
    g_source_remove (self->upload_retry_source);
  self->upload_retry_source = g_timeout_add_seconds_full (
    G_PRIORITY_DEFAULT, MAX (1u, delay), retry_pending_skin_upload,
    nuvashalauncher_skin_service_ref (self),
    (GDestroyNotify) nuvashalauncher_skin_service_unref);
}

void
nuvashalauncher_skin_service_upload_skin (NuvashaLauncherSkinService *self,
                                         const char                 *url,
                                         const char                 *variant)
{
  if (self == NULL || url == NULL ||
      (!g_str_has_prefix (url, "file://") &&
       !g_str_has_prefix (url, "http://") &&
       !g_str_has_prefix (url, "https://")))
    return;

  if (self->upload_in_flight && self->upload_cancellable != NULL)
    g_cancellable_cancel (self->upload_cancellable);
  g_free (self->pending_upload_url);
  self->pending_upload_url = g_strdup (url);
  g_free (self->pending_upload_variant);
  self->pending_upload_variant = g_strdup (
    variant != NULL && g_ascii_strcasecmp (variant, "SLIM") == 0
      ? "SLIM" : "CLASSIC");

  if (!self->upload_in_flight)
    start_pending_skin_upload (self);
}

static void
on_active_skin_delete_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                            gpointer user_data)
{
  NuvashaLauncherSkinService *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_task_propagate_pointer (G_TASK (result), &error);

  if (skin_background_operations > 0)
    skin_background_operations--;

  if (bytes != NULL && error == NULL)
    {
      if (self->callbacks.on_delete_succeeded != NULL)
        self->callbacks.on_delete_succeeded (self->user_data);
    }
  else
    {
      if (error != NULL &&
          error->domain == nuvashalauncher_MINECRAFT_SERVICES_ERROR &&
          error->code == nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED)
        {
          nuvashalauncher_skin_service_unref (self);
          return;
        }
      g_warning ("%s", nuvashalauncher_error_message (error));
      notify_error (self, error);
    }

  nuvashalauncher_skin_service_unref (self);
}

static void
send_pending_cape_change (NuvashaLauncherSkinService *self)
{
  SoupMessage *message;
  g_autofree char *json = NULL;
  g_autofree char *cape_id = NULL;

  if (self == NULL || self->pending_cape_id == NULL)
    return;
  if (self->account == NULL || self->account->access_token == NULL ||
      !nuvashalauncher_account_token_is_valid (self->account, g_get_real_time () / 1000000))
    return;
  cape_id = g_steal_pointer (&self->pending_cape_id);
  message = nuvashalauncher_minecraft_services_message (
    cape_id != NULL && *cape_id != '\0' ? SOUP_METHOD_PUT : SOUP_METHOD_DELETE,
    "/minecraft/profile/capes/active", self->account->access_token);
  if (cape_id != NULL && *cape_id != '\0')
    {
      g_autoptr (GBytes) body_bytes = NULL;
      json = g_strdup_printf ("{\"capeId\":\"%s\"}", cape_id);
      body_bytes = g_bytes_new (json, strlen (json));
      soup_message_set_request_body_from_bytes (message, "application/json",
                                                body_bytes);
    }
  skin_background_operations++;
  nuvashalauncher_task_scope_unref_when_done (
    nuvashalauncher_minecraft_services_send_async (message, NULL,
                                                   on_active_skin_delete_done,
                                                   nuvashalauncher_skin_service_ref (self)));
  g_object_unref (message);
}

void
nuvashalauncher_skin_service_set_cape (NuvashaLauncherSkinService *self,
                                        const char                 *cape_id)
{
  if (self == NULL)
    return;

  g_free (self->pending_cape_id);
  self->pending_cape_id = g_strdup (cape_id != NULL ? cape_id : "");

  if (self->upload_in_flight)
    return;

  send_pending_cape_change (self);
}

void
nuvashalauncher_skin_service_delete_active_skin (NuvashaLauncherSkinService *self,
                                                  GCancellable               *cancellable)
{
  SoupMessage *message;

  if (self == NULL || self->account == NULL ||
      self->account->type != nuvashalauncher_ACCOUNT_MICROSOFT ||
      self->account->access_token == NULL)
    return;

  message = nuvashalauncher_minecraft_services_message (
    SOUP_METHOD_DELETE, "/minecraft/profile/skins/active",
    self->account->access_token);
  skin_background_operations++;
  nuvashalauncher_task_scope_unref_when_done (
    nuvashalauncher_minecraft_services_send_async (message, cancellable,
                                                   on_active_skin_delete_done,
                                                   nuvashalauncher_skin_service_ref (self)));
  g_object_unref (message);
}
