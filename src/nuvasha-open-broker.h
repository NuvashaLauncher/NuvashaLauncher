/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_OPEN_BROKER_H
#define nuvasha_OPEN_BROKER_H

#include <gio/gio.h>

G_BEGIN_DECLS

const char *nuvasha_open_broker_start (const char *allowed_root, GError **error);
int nuvasha_open_broker_request (const char *socket_path, const char *uri);

G_END_DECLS

#endif /* nuvasha_OPEN_BROKER_H */
