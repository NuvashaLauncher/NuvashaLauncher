/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherUsersPopover: account selector shown in the window header.
 * It follows nuvashalauncherAccounts and rebuilds when the account list or active
 * account changes.
 *
 * An offline account is a name and nothing else, so it says "Offline"
 * wherever it is shown and has no avatar. A Microsoft account carries the
 * name and the UUID the services gave, and its skin is fetched and shown
 * as the avatar when there is one.
 *
 * Adding a Microsoft account opens the device-code sign-in dialog. Adding an
 * offline account opens a dialog that asks for a name and checks it is
 * one the game accepts. */

#ifndef nuvashalauncher_USERS_POPOVER_H
#define nuvashalauncher_USERS_POPOVER_H

#include <adwaita.h>

#include "nuvashalauncher-accounts.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_USERS_POPOVER (nuvashalauncher_users_popover_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherUsersPopover, nuvashalauncher_users_popover, nuvashalauncher, USERS_POPOVER,
                      GtkPopover)

/* The popover of @accounts, and it takes its own reference on them. */
nuvashalauncherUsersPopover *nuvashalauncher_users_popover_new (nuvashalauncherAccounts *accounts);

/* The store this shows. Borrowed. */
nuvashalauncherAccounts *nuvashalauncher_users_popover_get_accounts (nuvashalauncherUsersPopover *self);

/* The name of the active account, the empty string when there is none.
   (transfer full) */
char *nuvashalauncher_users_popover_get_active_name (nuvashalauncherUsersPopover *self);

/* Return header-button content for the active account. The caller owns it. */
GtkWidget *nuvashalauncher_users_popover_get_button_content (nuvashalauncherUsersPopover *self);

/* TRUE while a skin or cape request is active or waiting to retry. */
gboolean nuvashalauncher_users_skin_background_work_active (void);

G_END_DECLS

#endif /* nuvashalauncher_USERS_POPOVER_H */
