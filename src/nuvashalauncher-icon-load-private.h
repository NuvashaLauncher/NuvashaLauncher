/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_ICON_LOAD_PRIVATE_H
#define nuvashalauncher_ICON_LOAD_PRIVATE_H

#include "nuvashalauncher-operation.h"

typedef struct
{
  GtkWidget *icon;
  nuvashalauncherTaskScope *operation;
} nuvashalauncherIconLoad;

void nuvashalauncher_icon_load_free (nuvashalauncherIconLoad *load);
void nuvashalauncher_icon_load_apply (nuvashalauncherIconLoad *load, GAsyncResult *result);
void nuvashalauncher_icon_load_ready (GObject *source, GAsyncResult *result,
                          gpointer user_data);
nuvashalauncherIconLoad *nuvashalauncher_icon_load_start (GtkWidget *icon, const char *url);

#endif /* nuvashalauncher_ICON_LOAD_PRIVATE_H */
