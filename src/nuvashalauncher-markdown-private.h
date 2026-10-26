/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MARKDOWN_PRIVATE_H
#define nuvashalauncher_MARKDOWN_PRIVATE_H

#include "nuvashalauncher-markdown.h"

char *nuvashalauncher_markdown_inline_allow_html (const char *text);
gboolean nuvashalauncher_markdown_url_is_safe (const char *url);

#endif
