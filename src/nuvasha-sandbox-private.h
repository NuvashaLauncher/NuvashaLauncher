/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_SANDBOX_PRIVATE_H
#define nuvasha_SANDBOX_PRIVATE_H

#include <glib.h>

void nuvasha_sandbox_add_argument (GPtrArray *arguments, const char *value);
void nuvasha_sandbox_add_directory (GPtrArray *arguments, const char *path);
void nuvasha_sandbox_bind_readonly_if_present (GPtrArray *arguments,
                                           const char *path);

#endif /* nuvasha_SANDBOX_PRIVATE_H */
