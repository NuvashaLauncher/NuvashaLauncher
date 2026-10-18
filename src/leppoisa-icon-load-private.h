/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_ICON_LOAD_PRIVATE_H
#define leppoisa_ICON_LOAD_PRIVATE_H

#include "leppoisa-operation.h"

typedef struct
{
  GtkWidget *icon;
  LeppoisaTaskScope *operation;
} LeppoisaIconLoad;

void leppoisa_icon_load_free (LeppoisaIconLoad *load);
void leppoisa_icon_load_apply (LeppoisaIconLoad *load, GAsyncResult *result);
void leppoisa_icon_load_ready (GObject *source, GAsyncResult *result,
                          gpointer user_data);
LeppoisaIconLoad *leppoisa_icon_load_start (GtkWidget *icon, const char *url);

#endif /* leppoisa_ICON_LOAD_PRIVATE_H */
