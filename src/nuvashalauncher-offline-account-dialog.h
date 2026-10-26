/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherOfflineAccountDialog: an offline account, which is a name.
 *
 * There is nothing to sign in to: an offline account is a player name and
 * the UUID java derives from that name, and the game is told the session
 * is not a real one. It is labelled as offline wherever it is shown.
 *
 * The name is what the game will call the player, so it is checked as
 * what the game accepts, 3 to 16 characters of letters, digits and
 * underscores, and the reason a name is refused is written under the
 * entry rather than only refusing the Add button: a name the user typed
 * wrong is not something to guess at. */

#ifndef nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_H
#define nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_H

#include <adwaita.h>

#include "nuvashalauncher-accounts.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_OFFLINE_ACCOUNT_DIALOG \
  (nuvashalauncher_offline_account_dialog_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherOfflineAccountDialog, nuvashalauncher_offline_account_dialog, nuvashalauncher,
                      OFFLINE_ACCOUNT_DIALOG, AdwDialog)

/* "account-added" carries the offline account, transfer full. */
enum {
  nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED,
  nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_N_SIGNALS
};

/* The dialog, which starts with an empty name and nothing added. */
nuvashalauncherOfflineAccountDialog *nuvashalauncher_offline_account_dialog_new (void);

/* The store is optional for template/tests, but enables duplicate-name
 * validation when the dialog is opened from the account popover. */
void nuvashalauncher_offline_account_dialog_set_accounts (nuvashalauncherOfflineAccountDialog *self,
                                                  nuvashalauncherAccounts *accounts);

G_END_DECLS

#endif /* nuvashalauncher_OFFLINE_ACCOUNT_DIALOG_H */
