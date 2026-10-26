/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_SKIN_VIEW_H
#define nuvashalauncher_SKIN_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_SKIN_VIEW (nuvashalauncher_skin_view_get_type ())
G_DECLARE_FINAL_TYPE (nuvashalauncherSkinView, nuvashalauncher_skin_view, nuvashalauncher, SKIN_VIEW, GtkGLArea)

nuvashalauncherSkinView *nuvashalauncher_skin_view_new (void);
void nuvashalauncher_skin_view_set_skin (nuvashalauncherSkinView *self, GdkTexture *texture);
void nuvashalauncher_skin_view_set_cape (nuvashalauncherSkinView *self, GdkTexture *texture);
void nuvashalauncher_skin_view_set_slim (nuvashalauncherSkinView *self, gboolean slim);

G_END_DECLS

#endif
