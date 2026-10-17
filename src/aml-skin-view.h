/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_SKIN_VIEW_H
#define AML_SKIN_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define AML_TYPE_SKIN_VIEW (aml_skin_view_get_type ())
G_DECLARE_FINAL_TYPE (AmlSkinView, aml_skin_view, AML, SKIN_VIEW, GtkGLArea)

AmlSkinView *aml_skin_view_new (void);
void aml_skin_view_set_skin (AmlSkinView *self, GdkTexture *texture);
void aml_skin_view_set_cape (AmlSkinView *self, GdkTexture *texture);
void aml_skin_view_set_slim (AmlSkinView *self, gboolean slim);

G_END_DECLS

#endif
