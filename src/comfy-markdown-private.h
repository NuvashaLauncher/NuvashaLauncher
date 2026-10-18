/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_MARKDOWN_PRIVATE_H
#define comfy_MARKDOWN_PRIVATE_H

#include "comfy-markdown.h"

char *comfy_markdown_inline_allow_html (const char *text);
gboolean comfy_markdown_url_is_safe (const char *url);

#endif
