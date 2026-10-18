/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_SKIN_VIEW_H
#define leppoisa_SKIN_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define leppoisa_TYPE_SKIN_VIEW (leppoisa_skin_view_get_type ())
G_DECLARE_FINAL_TYPE (LeppoisaSkinView, leppoisa_skin_view, leppoisa, SKIN_VIEW, GtkGLArea)

LeppoisaSkinView *leppoisa_skin_view_new (void);
void leppoisa_skin_view_set_skin (LeppoisaSkinView *self, GdkTexture *texture);
void leppoisa_skin_view_set_cape (LeppoisaSkinView *self, GdkTexture *texture);
void leppoisa_skin_view_set_slim (LeppoisaSkinView *self, gboolean slim);

G_END_DECLS

#endif
