/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_WORLDS_PAGE_H
#define NUVASHALAUNCHER_WORLDS_PAGE_H

#include <adwaita.h>

#include "nuvashalauncher-instance.h"
#include "nuvashalauncher-launcher.h"

G_BEGIN_DECLS

#define NUVASHALAUNCHER_TYPE_WORLDS_PAGE (nuvashalauncher_worlds_page_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLauncherWorldsPage,
                      nuvashalauncher_worlds_page,
                      NUVASHALAUNCHER,
                      WORLDS_PAGE,
                      GtkBox)

NuvashaLauncherWorldsPage *nuvashalauncher_worlds_page_new (
    NuvashaLauncherInstance *instance,
    NuvashaLauncher *launcher);

void nuvashalauncher_worlds_page_reload (NuvashaLauncherWorldsPage *self);
void nuvashalauncher_worlds_page_attach_header (NuvashaLauncherWorldsPage *self,
                                                AdwHeaderBar *header);
void nuvashalauncher_worlds_page_detach_header (NuvashaLauncherWorldsPage *self,
                                                AdwHeaderBar *header);
void nuvashalauncher_worlds_page_set_search_visible (NuvashaLauncherWorldsPage *self,
                                                     gboolean visible);
gboolean nuvashalauncher_worlds_page_is_search_visible (NuvashaLauncherWorldsPage *self);

G_END_DECLS

#endif
