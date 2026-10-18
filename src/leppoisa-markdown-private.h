/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_MARKDOWN_PRIVATE_H
#define leppoisa_MARKDOWN_PRIVATE_H

#include "leppoisa-markdown.h"

char *leppoisa_markdown_inline_allow_html (const char *text);
gboolean leppoisa_markdown_url_is_safe (const char *url);

#endif
