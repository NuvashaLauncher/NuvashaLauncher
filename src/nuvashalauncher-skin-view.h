/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_SKIN_VIEW_H
#define nuvashalauncher_SKIN_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_SKIN_VIEW (nuvashalauncher_skin_view_get_type ())
G_DECLARE_FINAL_TYPE (NuvashaLauncherSkinView, nuvashalauncher_skin_view, nuvashalauncher, SKIN_VIEW, GtkGLArea)

NuvashaLauncherSkinView *nuvashalauncher_skin_view_new (void);
void nuvashalauncher_skin_view_set_skin (NuvashaLauncherSkinView *self, GdkTexture *texture);
void nuvashalauncher_skin_view_set_cape (NuvashaLauncherSkinView *self, GdkTexture *texture);
void nuvashalauncher_skin_view_set_slim (NuvashaLauncherSkinView *self, gboolean slim);

/* Return a PNG suitable for uploading as a classic skin.  Slim skin atlases
 * need their missing classic arm columns filled before Mojang's services
 * consume them. */
GBytes *nuvashalauncher_skin_prepare_classic_upload (GBytes *skin);

G_END_DECLS

#endif
