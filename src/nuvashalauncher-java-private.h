/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_JAVA_PRIVATE_H
#define NUVASHALAUNCHER_JAVA_PRIVATE_H

#include "nuvashalauncher-java.h"

gboolean java_has_xawt (const char *executable);
char *real_path (const char *path);

#endif
