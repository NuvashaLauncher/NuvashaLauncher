/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_SKIN_VIEW_H
#define nuvasha_SKIN_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define nuvasha_TYPE_SKIN_VIEW (nuvasha_skin_view_get_type ())
G_DECLARE_FINAL_TYPE (NuvashaSkinView, nuvasha_skin_view, nuvasha, SKIN_VIEW, GtkGLArea)

NuvashaSkinView *nuvasha_skin_view_new (void);
void nuvasha_skin_view_set_skin (NuvashaSkinView *self, GdkTexture *texture);
void nuvasha_skin_view_set_cape (NuvashaSkinView *self, GdkTexture *texture);
void nuvasha_skin_view_set_slim (NuvashaSkinView *self, gboolean slim);

G_END_DECLS

#endif
