/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherSignInDialog: signing in with a Microsoft account.
 *
 * The user reads a short code and types it into a page Microsoft serves,
 * in a browser of their own, while this dialog waits. No password is ever
 * asked for here or anywhere else in nuvashalauncher: what the browser takes is
 * between the user and Microsoft, and what comes back is a token.
 *
 * The dialog opens the page for the user with GIO, the same way the rest
 * of the desktop opens a link, and shows the address as well, so a user
 * who would rather open it themselves can.
 *
 * What the chain answers decides what the dialog says. An account that
 * does not own Minecraft: Java is refused before the profile is asked
 * for, and an account that owns it and has never played has no profile
 * yet: both are said in words rather than as a sign-in that appeared to
 * work. A refusal the user can do something about, like a child account
 * that has to be added to a family, is said as such.
 *
 * The account that comes back is emitted as "signed-in" and is the
 * caller's to store: this dialog stores nothing. */

#ifndef nuvashalauncher_SIGN_IN_DIALOG_H
#define nuvashalauncher_SIGN_IN_DIALOG_H

#include <adwaita.h>

#include "nuvashalauncher-accounts.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_SIGN_IN_DIALOG (nuvashalauncher_sign_in_dialog_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherSignInDialog, nuvashalauncher_sign_in_dialog, nuvashalauncher, SIGN_IN_DIALOG,
                      AdwDialog)

/* "signed-in" carries the account that was signed in to, transfer full. */
enum {
  nuvashalauncher_SIGN_IN_DIALOG_SIGNAL_SIGNED_IN,
  nuvashalauncher_SIGN_IN_DIALOG_N_SIGNALS
};

/* The dialog. It asks Microsoft for a code as soon as it is made, so that
   the code is there by the time a user looks at it, and follows the
   sign-in from there. A dialog that is made and never shown has asked for
   one code and stopped there. */
nuvashalauncherSignInDialog *nuvashalauncher_sign_in_dialog_new (void);

/* Asks for a new code and starts over, which is what the Try again button
   does after a refusal the user can fix. */
void nuvashalauncher_sign_in_dialog_retry (nuvashalauncherSignInDialog *self);

G_END_DECLS

#endif /* nuvashalauncher_SIGN_IN_DIALOG_H */
