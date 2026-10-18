/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_SKIN_VIEW_H
#define comfy_SKIN_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define comfy_TYPE_SKIN_VIEW (comfy_skin_view_get_type ())
G_DECLARE_FINAL_TYPE (ComfySkinView, comfy_skin_view, comfy, SKIN_VIEW, GtkGLArea)

ComfySkinView *comfy_skin_view_new (void);
void comfy_skin_view_set_skin (ComfySkinView *self, GdkTexture *texture);
void comfy_skin_view_set_cape (ComfySkinView *self, GdkTexture *texture);
void comfy_skin_view_set_slim (ComfySkinView *self, gboolean slim);

G_END_DECLS

#endif
