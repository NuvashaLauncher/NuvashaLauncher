/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_ICON_LOAD_PRIVATE_H
#define nuvashalauncher_ICON_LOAD_PRIVATE_H

#include "nuvashalauncher-operation.h"

typedef struct
{
  GtkWidget *icon;
  NuvashaLauncherTaskScope *operation;
} NuvashaLauncherIconLoad;

void nuvashalauncher_icon_load_free (NuvashaLauncherIconLoad *load);
void nuvashalauncher_icon_load_apply (NuvashaLauncherIconLoad *load, GAsyncResult *result);
void nuvashalauncher_icon_load_ready (GObject *source, GAsyncResult *result,
                          gpointer user_data);
NuvashaLauncherIconLoad *nuvashalauncher_icon_load_start (GtkWidget *icon, const char *url);

#endif /* nuvashalauncher_ICON_LOAD_PRIVATE_H */
