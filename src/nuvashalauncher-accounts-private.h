#ifndef nuvashalauncher_ACCOUNTS_PRIVATE_H
#define nuvashalauncher_ACCOUNTS_PRIVATE_H

#include "nuvashalauncher-accounts.h"

void account_to_json (JsonBuilder *builder,
                      const nuvashalauncherAccount *account);
nuvashalauncherAccount *account_from_json (JsonObject *object);
char *string_member (JsonObject *object, const char *name);

#endif
