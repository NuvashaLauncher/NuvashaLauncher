/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHA_SKIN_SERVICE_H
#define NUVASHA_SKIN_SERVICE_H

#include "nuvashalauncher-accounts.h"
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NuvashaLauncherSkinService NuvashaLauncherSkinService;

typedef struct
{
  void (*on_upload_succeeded) (gpointer user_data);
  void (*on_delete_succeeded) (gpointer user_data);
  void (*on_error)            (const GError *error, gpointer user_data);
} NuvashaLauncherSkinServiceCallbacks;

NuvashaLauncherSkinService *nuvashalauncher_skin_service_new (
  const NuvashaLauncherAccount              *account,
  const NuvashaLauncherSkinServiceCallbacks *callbacks,
  gpointer                                   user_data);

NuvashaLauncherSkinService *nuvashalauncher_skin_service_ref   (NuvashaLauncherSkinService *self);
void                        nuvashalauncher_skin_service_unref (NuvashaLauncherSkinService *self);

/* Clears UI callbacks so in-flight background uploads/retries can finish
 * safely after the owning dialog closes. */
void nuvashalauncher_skin_service_detach_ui (NuvashaLauncherSkinService *self);

void nuvashalauncher_skin_service_upload_skin (NuvashaLauncherSkinService *self,
                                               const char                 *url,
                                               const char                 *variant);

void nuvashalauncher_skin_service_set_cape (NuvashaLauncherSkinService *self,
                                            const char                 *cape_id);

void nuvashalauncher_skin_service_delete_active_skin (NuvashaLauncherSkinService *self,
                                                      GCancellable               *cancellable);

gboolean nuvashalauncher_skin_service_background_work_active (void);

G_END_DECLS

#endif /* NUVASHA_SKIN_SERVICE_H */
