/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_UI_FEEDBACK_H
#define leppoisa_UI_FEEDBACK_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Shows a plain-text toast while @page is still attached to a window. */
void leppoisa_ui_show_toast (GtkWidget  *page,
                        GtkWidget  *overlay,
                        const char *message);

GtkWidget *leppoisa_ui_loading_child (void);
void leppoisa_ui_clear_error_label (GtkLabel *label);
void leppoisa_ui_cancel_dialog (GtkButton *button, gpointer user_data);

G_END_DECLS

#endif /* leppoisa_UI_FEEDBACK_H */
