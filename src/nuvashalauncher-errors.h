/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef nuvashalauncher_ERRORS_H
#define nuvashalauncher_ERRORS_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Localized user-facing summary. The original GError remains diagnostic data. */
const char *nuvashalauncher_error_message (const GError *error);
gboolean nuvashalauncher_error_is_cancelled (const GError *error);

G_END_DECLS
#endif
