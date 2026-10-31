/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_UI_FEEDBACK_H
#define nuvashalauncher_UI_FEEDBACK_H

#include <adwaita.h>
#include "nuvashalauncher-errors.h"

G_BEGIN_DECLS

/* Shows a plain-text toast while @page is still attached to a window. */
void nuvashalauncher_ui_show_toast (GtkWidget  *page,
                        GtkWidget  *overlay,
                        const char *message);

GtkWidget *nuvashalauncher_ui_loading_child (void);
void nuvashalauncher_ui_clear_error_label (GtkLabel *label);
void nuvashalauncher_ui_cancel_dialog (GtkButton *button, gpointer user_data);
void nuvashalauncher_ui_show_error (GtkWidget *parent, const char *title,
                                   const GError *error);
GtkWidget *nuvashalauncher_ui_error_details (const GError *error);
void nuvashalauncher_ui_error_label (GtkLabel *label, const GError *error);
void nuvashalauncher_ui_error_banner (AdwBanner *banner, const GError *error);
void nuvashalauncher_ui_error_toast (GtkWidget *page, GtkWidget *overlay,
                                    const GError *error);

/* Shared presentation helpers for icon-only controls and dialog surfaces. */
void nuvashalauncher_ui_set_action_label (GtkWidget *button, const char *label);
void nuvashalauncher_ui_prepare_icon_button (GtkWidget *button, const char *label);
void nuvashalauncher_ui_prepare_dialog_content (GtkWidget *content);

G_END_DECLS

#endif /* nuvashalauncher_UI_FEEDBACK_H */
