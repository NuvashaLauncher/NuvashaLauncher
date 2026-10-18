/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_UI_FEEDBACK_H
#define comfy_UI_FEEDBACK_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Shows a plain-text toast while @page is still attached to a window. */
void comfy_ui_show_toast (GtkWidget  *page,
                        GtkWidget  *overlay,
                        const char *message);

GtkWidget *comfy_ui_loading_child (void);
void comfy_ui_clear_error_label (GtkLabel *label);
void comfy_ui_cancel_dialog (GtkButton *button, gpointer user_data);

G_END_DECLS

#endif /* comfy_UI_FEEDBACK_H */
