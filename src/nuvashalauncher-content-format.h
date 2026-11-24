/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_CONTENT_FORMAT_H
#define NUVASHALAUNCHER_CONTENT_FORMAT_H

#include <glib.h>

G_BEGIN_DECLS

/* (transfer full) A short, localized download count. */
char *nuvashalauncher_content_format_downloads (guint64 downloads);

/* (transfer full) A localized author/license line. */
char *nuvashalauncher_content_format_author (const char *license, const char *author);

G_END_DECLS

#endif /* NUVASHALAUNCHER_CONTENT_FORMAT_H */
