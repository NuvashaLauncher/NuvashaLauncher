/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_MARKDOWN_PRIVATE_H
#define nuvasha_MARKDOWN_PRIVATE_H

#include "nuvasha-markdown.h"

char *nuvasha_markdown_inline_allow_html (const char *text);
gboolean nuvasha_markdown_url_is_safe (const char *url);

#endif
