/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaUsersPopover: account selector shown in the window header.
 * It follows NuvashaAccounts and rebuilds when the account list or active
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

#ifndef nuvasha_USERS_POPOVER_H
#define nuvasha_USERS_POPOVER_H

#include <adwaita.h>

#include "nuvasha-accounts.h"

G_BEGIN_DECLS

#define nuvasha_TYPE_USERS_POPOVER (nuvasha_users_popover_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaUsersPopover, nuvasha_users_popover, nuvasha, USERS_POPOVER,
                      GtkPopover)

/* The popover of @accounts, and it takes its own reference on them. */
NuvashaUsersPopover *nuvasha_users_popover_new (NuvashaAccounts *accounts);

/* The store this shows. Borrowed. */
NuvashaAccounts *nuvasha_users_popover_get_accounts (NuvashaUsersPopover *self);

/* The name of the active account, the empty string when there is none.
   (transfer full) */
char *nuvasha_users_popover_get_active_name (NuvashaUsersPopover *self);

/* Return header-button content for the active account. The caller owns it. */
GtkWidget *nuvasha_users_popover_get_button_content (NuvashaUsersPopover *self);

G_END_DECLS

#endif /* nuvasha_USERS_POPOVER_H */
