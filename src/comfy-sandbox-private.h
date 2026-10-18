/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_SANDBOX_PRIVATE_H
#define comfy_SANDBOX_PRIVATE_H

#include <glib.h>

void comfy_sandbox_add_argument (GPtrArray *arguments, const char *value);
void comfy_sandbox_add_directory (GPtrArray *arguments, const char *path);
void comfy_sandbox_bind_readonly_if_present (GPtrArray *arguments,
                                           const char *path);

#endif /* comfy_SANDBOX_PRIVATE_H */
