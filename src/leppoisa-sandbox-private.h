/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_SANDBOX_PRIVATE_H
#define leppoisa_SANDBOX_PRIVATE_H

#include <glib.h>

void leppoisa_sandbox_add_argument (GPtrArray *arguments, const char *value);
void leppoisa_sandbox_add_directory (GPtrArray *arguments, const char *path);
void leppoisa_sandbox_bind_readonly_if_present (GPtrArray *arguments,
                                           const char *path);

#endif /* leppoisa_SANDBOX_PRIVATE_H */
