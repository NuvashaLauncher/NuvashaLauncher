/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_ICON_LOAD_PRIVATE_H
#define nuvasha_ICON_LOAD_PRIVATE_H

#include "nuvasha-operation.h"

typedef struct
{
  GtkWidget *icon;
  NuvashaTaskScope *operation;
} NuvashaIconLoad;

void nuvasha_icon_load_free (NuvashaIconLoad *load);
void nuvasha_icon_load_apply (NuvashaIconLoad *load, GAsyncResult *result);
void nuvasha_icon_load_ready (GObject *source, GAsyncResult *result,
                          gpointer user_data);
NuvashaIconLoad *nuvasha_icon_load_start (GtkWidget *icon, const char *url);

#endif /* nuvasha_ICON_LOAD_PRIVATE_H */
