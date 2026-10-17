/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_SANDBOX_PRIVATE_H
#define AML_SANDBOX_PRIVATE_H

#include <glib.h>

void aml_sandbox_add_argument (GPtrArray *arguments, const char *value);
void aml_sandbox_add_directory (GPtrArray *arguments, const char *path);
void aml_sandbox_bind_readonly_if_present (GPtrArray *arguments,
                                           const char *path);

#endif /* AML_SANDBOX_PRIVATE_H */
