/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_CONTENT_FORMAT_H
#define nuvasha_CONTENT_FORMAT_H

#include <glib.h>

G_BEGIN_DECLS

/* (transfer full) A short, localized download count. */
char *nuvasha_content_format_downloads (guint64 downloads);

/* (transfer full) A localized author/license line. */
char *nuvasha_content_format_author (const char *license, const char *author);

G_END_DECLS

#endif /* nuvasha_CONTENT_FORMAT_H */
