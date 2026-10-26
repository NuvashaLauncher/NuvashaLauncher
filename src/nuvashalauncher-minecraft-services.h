/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MINECRAFT_SERVICES_H
#define nuvashalauncher_MINECRAFT_SERVICES_H

#include <libsoup/soup.h>
#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

#define nuvashalauncher_MINECRAFT_SERVICES_ERROR (nuvashalauncher_minecraft_services_error_quark ())

typedef enum {
  nuvashalauncher_MINECRAFT_SERVICES_ERROR_NETWORK,
  nuvashalauncher_MINECRAFT_SERVICES_ERROR_HTTP,
  nuvashalauncher_MINECRAFT_SERVICES_ERROR_RESPONSE,
  nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED
} nuvashalauncherMinecraftServicesError;

GQuark nuvashalauncher_minecraft_services_error_quark (void);

SoupSession *nuvashalauncher_minecraft_services_session (void);
SoupMessage *nuvashalauncher_minecraft_services_message (const char *method,
                                                 const char *path,
                                                 const char *access_token);

/* Returns the server delay in seconds, honoring Retry-After when present. */
guint nuvashalauncher_minecraft_services_retry_delay (SoupMessage *message,
                                              guint attempt);

GError *nuvashalauncher_minecraft_services_response_error (SoupMessage *message,
                                                   GBytes *body);

GBytes *nuvashalauncher_minecraft_services_skin_multipart (GBytes *skin,
                                                   const char *variant,
                                                   char **content_type);

nuvashalauncherTaskScope *nuvashalauncher_minecraft_services_send_async (SoupMessage *message,
                                                         GCancellable *cancellable,
                                                         GAsyncReadyCallback callback,
                                                         gpointer user_data);

G_END_DECLS

#endif /* nuvashalauncher_MINECRAFT_SERVICES_H */
