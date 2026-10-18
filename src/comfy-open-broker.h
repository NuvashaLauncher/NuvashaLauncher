/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_OPEN_BROKER_H
#define comfy_OPEN_BROKER_H

#include <gio/gio.h>

G_BEGIN_DECLS

const char *comfy_open_broker_start (const char *allowed_root, GError **error);
int comfy_open_broker_request (const char *socket_path, const char *uri);

G_END_DECLS

#endif /* comfy_OPEN_BROKER_H */
