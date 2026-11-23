/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_ACCOUNTS_PRIVATE_H
#define NUVASHALAUNCHER_ACCOUNTS_PRIVATE_H

#include "nuvashalauncher-accounts.h"

void account_to_json (JsonBuilder *builder,
                      const NuvashaLauncherAccount *account);
NuvashaLauncherAccount *account_from_json (JsonObject *object);
char *string_member (JsonObject *object, const char *name);

#endif
