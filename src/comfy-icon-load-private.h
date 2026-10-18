/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_ICON_LOAD_PRIVATE_H
#define comfy_ICON_LOAD_PRIVATE_H

#include "comfy-operation.h"

typedef struct
{
  GtkWidget *icon;
  ComfyTaskScope *operation;
} ComfyIconLoad;

void comfy_icon_load_free (ComfyIconLoad *load);
void comfy_icon_load_apply (ComfyIconLoad *load, GAsyncResult *result);
void comfy_icon_load_ready (GObject *source, GAsyncResult *result,
                          gpointer user_data);
ComfyIconLoad *comfy_icon_load_start (GtkWidget *icon, const char *url);

#endif /* comfy_ICON_LOAD_PRIVATE_H */
