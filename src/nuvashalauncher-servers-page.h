/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_SERVERS_PAGE_H
#define NUVASHALAUNCHER_SERVERS_PAGE_H

#include <adwaita.h>

#include "nuvashalauncher-instance.h"
#include "nuvashalauncher-launcher.h"

G_BEGIN_DECLS

#define NUVASHALAUNCHER_TYPE_SERVERS_PAGE (nuvashalauncher_servers_page_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLauncherServersPage,
                      nuvashalauncher_servers_page,
                      NUVASHALAUNCHER,
                      SERVERS_PAGE,
                      GtkBox)

NuvashaLauncherServersPage *nuvashalauncher_servers_page_new (
    NuvashaLauncherInstance *instance,
    NuvashaLauncher *launcher);

void nuvashalauncher_servers_page_reload (NuvashaLauncherServersPage *self);
void nuvashalauncher_servers_page_attach_header (NuvashaLauncherServersPage *self,
                                                 AdwHeaderBar *header);
void nuvashalauncher_servers_page_detach_header (NuvashaLauncherServersPage *self,
                                                 AdwHeaderBar *header);
void nuvashalauncher_servers_page_set_search_visible (NuvashaLauncherServersPage *self,
                                                      gboolean visible);
gboolean nuvashalauncher_servers_page_is_search_visible (NuvashaLauncherServersPage *self);

G_END_DECLS

#endif
