/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_MINECRAFT_SERVICES_H
#define nuvasha_MINECRAFT_SERVICES_H

#include <libsoup/soup.h>
#include "nuvasha-task.h"

G_BEGIN_DECLS

#define nuvasha_MINECRAFT_SERVICES_ERROR (nuvasha_minecraft_services_error_quark ())

typedef enum {
  nuvasha_MINECRAFT_SERVICES_ERROR_NETWORK,
  nuvasha_MINECRAFT_SERVICES_ERROR_HTTP,
  nuvasha_MINECRAFT_SERVICES_ERROR_RESPONSE,
  nuvasha_MINECRAFT_SERVICES_ERROR_RATE_LIMITED
} NuvashaMinecraftServicesError;

GQuark nuvasha_minecraft_services_error_quark (void);

SoupSession *nuvasha_minecraft_services_session (void);
SoupMessage *nuvasha_minecraft_services_message (const char *method,
                                                 const char *path,
                                                 const char *access_token);

/* Returns the server delay in seconds, honoring Retry-After when present. */
guint nuvasha_minecraft_services_retry_delay (SoupMessage *message,
                                              guint attempt);

GError *nuvasha_minecraft_services_response_error (SoupMessage *message,
                                                   GBytes *body);

GBytes *nuvasha_minecraft_services_skin_multipart (GBytes *skin,
                                                   const char *variant,
                                                   char **content_type);

NuvashaTaskScope *nuvasha_minecraft_services_send_async (SoupMessage *message,
                                                         GCancellable *cancellable,
                                                         GAsyncReadyCallback callback,
                                                         gpointer user_data);

G_END_DECLS

#endif /* nuvasha_MINECRAFT_SERVICES_H */
