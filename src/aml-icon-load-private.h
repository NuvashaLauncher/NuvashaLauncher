/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_ICON_LOAD_PRIVATE_H
#define AML_ICON_LOAD_PRIVATE_H

#include "aml-operation.h"

typedef struct
{
  GtkWidget *icon;
  AmlTaskScope *operation;
} AmlIconLoad;

void aml_icon_load_free (AmlIconLoad *load);
void aml_icon_load_apply (AmlIconLoad *load, GAsyncResult *result);
void aml_icon_load_ready (GObject *source, GAsyncResult *result,
                          gpointer user_data);
AmlIconLoad *aml_icon_load_start (GtkWidget *icon, const char *url);

#endif /* AML_ICON_LOAD_PRIVATE_H */
