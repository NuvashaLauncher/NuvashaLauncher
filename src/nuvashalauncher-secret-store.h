/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_SECRET_STORE_H
#define nuvashalauncher_SECRET_STORE_H

#include "nuvashalauncher-accounts.h"

G_BEGIN_DECLS

gboolean nuvashalauncher_secret_store_load (GPtrArray **accounts,
                                             char      **active_id,
                                             GError    **error);
gboolean nuvashalauncher_secret_store_save_account (const nuvashalauncherAccount *account,
                                                     GError **error);
gboolean nuvashalauncher_secret_store_save_active (const char *account_id,
                                                    GError **error);
gboolean nuvashalauncher_secret_store_remove_account (const char *account_id,
                                                       GError **error);
gboolean nuvashalauncher_secret_store_clear_active (GError **error);

G_END_DECLS

#endif
