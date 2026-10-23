/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The sizes the dialogs that hold a list and a form are given.
 *
 * Create Instance uses the standard size. Download Modpack also uses the
 * shared height but has a wide variant for its two-pane browser. A dialog
 * that has to be a different size, such as the sign-in form that is one
 * column of a code, sets its own and does not use these. */

#ifndef nuvasha_DIALOGS_H
#define nuvasha_DIALOGS_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Wide enough for a row of results with a description under it, and tall
 * enough for a list plus the buttons below it, so a dialog of this kind
 * opens without a scrollbar on a laptop and does not fill a desktop. */
#define nuvasha_DIALOG_CONTENT_WIDTH 720
#define nuvasha_DIALOG_WIDE_CONTENT_WIDTH 1040
#define nuvasha_DIALOG_CONTENT_HEIGHT 560

/* Gives @dialog the size the dialogs of this kind share. Both width and
 * height are set, so no dialog of this kind follows its content and ends
 * up a different size from its twin. */
static inline void
nuvasha_dialog_set_standard_size (AdwDialog *dialog)
{
  g_return_if_fail (ADW_IS_DIALOG (dialog));

  adw_dialog_set_content_width (dialog, nuvasha_DIALOG_CONTENT_WIDTH);
  adw_dialog_set_content_height (dialog, nuvasha_DIALOG_CONTENT_HEIGHT);
}

/* AdwBreakpoint takes ownership of its condition. Keep that transfer in one
 * helper so every narrow dialog follows the same lifetime rule. */
static inline void
nuvasha_dialog_add_content_margin_breakpoint (AdwDialog *dialog,
                                              GtkWidget *content,
                                              int        max_width,
                                              int        margin)
{
  AdwBreakpointCondition *condition;
  g_autoptr (AdwBreakpoint) breakpoint;
  GValue value = G_VALUE_INIT;

  g_return_if_fail (ADW_IS_DIALOG (dialog));
  g_return_if_fail (GTK_IS_WIDGET (content));

  condition = adw_breakpoint_condition_new_length (
    ADW_BREAKPOINT_CONDITION_MAX_WIDTH, max_width, ADW_LENGTH_UNIT_PX);
  breakpoint = adw_breakpoint_new (condition);

  g_value_init (&value, G_TYPE_INT);
  g_value_set_int (&value, margin);
  adw_breakpoint_add_setter (breakpoint, G_OBJECT (content),
                             "margin-start", &value);
  adw_breakpoint_add_setter (breakpoint, G_OBJECT (content),
                             "margin-end", &value);
  adw_breakpoint_add_setter (breakpoint, G_OBJECT (content),
                             "margin-top", &value);
  adw_breakpoint_add_setter (breakpoint, G_OBJECT (content),
                             "margin-bottom", &value);
  g_value_unset (&value);
  adw_dialog_add_breakpoint (dialog, g_steal_pointer (&breakpoint));
}

G_END_DECLS

#endif /* nuvasha_DIALOGS_H */
