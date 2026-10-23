/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_SECRET_STORE_H
#define nuvasha_SECRET_STORE_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nuvasha_secret_store_save (const char *account_id,
                                    const char *access_token,
                                    const char *refresh_token);
gboolean nuvasha_secret_store_load (const char *account_id,
                                    char      **access_token,
                                    char      **refresh_token);
void     nuvasha_secret_store_clear (const char *account_id);

G_END_DECLS

#endif
