/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlOfflineAccountDialog: an offline account, which is a name.
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

#ifndef AML_OFFLINE_ACCOUNT_DIALOG_H
#define AML_OFFLINE_ACCOUNT_DIALOG_H

#include <adwaita.h>

#include "aml-accounts.h"

G_BEGIN_DECLS

#define AML_TYPE_OFFLINE_ACCOUNT_DIALOG \
  (aml_offline_account_dialog_get_type ())

G_DECLARE_FINAL_TYPE (AmlOfflineAccountDialog, aml_offline_account_dialog, AML,
                      OFFLINE_ACCOUNT_DIALOG, AdwDialog)

/* "account-added" carries the offline account, transfer full. */
enum {
  AML_OFFLINE_ACCOUNT_DIALOG_SIGNAL_ACCOUNT_ADDED,
  AML_OFFLINE_ACCOUNT_DIALOG_N_SIGNALS
};

/* The dialog, which starts with an empty name and nothing added. */
AmlOfflineAccountDialog *aml_offline_account_dialog_new (void);

G_END_DECLS

#endif /* AML_OFFLINE_ACCOUNT_DIALOG_H */
