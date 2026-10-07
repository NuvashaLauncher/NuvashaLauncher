/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The size the dialogs that hold a list and a form are given.
 *
 * Create Instance and Download Modpack are two ways of arriving at the
 * same kind of thing, so they are sized once here: two literals in two
 * files is a pair that drifts the first time one of them is changed. A
 * dialog that has to be a different size, such as the sign-in form that
 * is one column of a code, sets its own and does not use this. */

#ifndef AML_DIALOGS_H
#define AML_DIALOGS_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Wide enough for a row of results with a description under it, and tall
 * enough for a list plus the buttons below it, so a dialog of this kind
 * opens without a scrollbar on a laptop and does not fill a desktop. */
#define AML_DIALOG_CONTENT_WIDTH 720
#define AML_DIALOG_CONTENT_HEIGHT 560

/* Gives @dialog the size the dialogs of this kind share. Both width and
 * height are set, so no dialog of this kind follows its content and ends
 * up a different size from its twin. */
static inline void
aml_dialog_set_standard_size (AdwDialog *dialog)
{
  g_return_if_fail (ADW_IS_DIALOG (dialog));

  adw_dialog_set_content_width (dialog, AML_DIALOG_CONTENT_WIDTH);
  adw_dialog_set_content_height (dialog, AML_DIALOG_CONTENT_HEIGHT);
}

G_END_DECLS

#endif /* AML_DIALOGS_H */