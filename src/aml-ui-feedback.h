/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_UI_FEEDBACK_H
#define AML_UI_FEEDBACK_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Shows a plain-text toast while @page is still attached to a window. */
void aml_ui_show_toast (GtkWidget  *page,
                        GtkWidget  *overlay,
                        const char *message);

G_END_DECLS

#endif /* AML_UI_FEEDBACK_H */
