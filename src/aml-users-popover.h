/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlUsersPopover: who this copy of AML plays as.
 *
 * The header of the window shows this as a button with an avatar and the
 * name of the active account, and this is what is behind it: every
 * account the user has, which one of them is active, and the two ways of
 * adding one.
 *
 * The accounts themselves are in aml-accounts.h and this follows that
 * store rather than keeping any of its own: what the popover shows is
 * rebuilt whenever the store says the accounts or the active one are not
 * what they were, which is also what happens after a sign-in.
 *
 * An offline account is a name and nothing else, so it says "Offline"
 * wherever it is shown and has no avatar. A Microsoft account carries the
 * name and the UUID the services gave, and its skin is fetched and shown
 * as the avatar when there is one.
 *
 * Adding a Microsoft account opens the sign-in dialog, which types a code
 * into a page of Microsoft's own and never asks for a password. Adding an
 * offline account opens a dialog that asks for a name and checks it is
 * one the game accepts. */

#ifndef AML_USERS_POPOVER_H
#define AML_USERS_POPOVER_H

#include <adwaita.h>

#include "aml-accounts.h"

G_BEGIN_DECLS

#define AML_TYPE_USERS_POPOVER (aml_users_popover_get_type ())

G_DECLARE_FINAL_TYPE (AmlUsersPopover, aml_users_popover, AML, USERS_POPOVER,
                      GtkPopover)

/* The popover of @accounts, and it takes its own reference on them. */
AmlUsersPopover *aml_users_popover_new (AmlAccounts *accounts);

/* The store this shows. Borrowed. */
AmlAccounts *aml_users_popover_get_accounts (AmlUsersPopover *self);

/* The name of the active account, the empty string when there is none.
   (transfer full) */
char *aml_users_popover_get_active_name (AmlUsersPopover *self);

/* What the menu button in the header shows: the avatar of the active
 * account and its name, and both of them follow the store. The caller
 * owns the returned widget. */
GtkWidget *aml_users_popover_get_button_content (AmlUsersPopover *self);

G_END_DECLS

#endif /* AML_USERS_POPOVER_H */
