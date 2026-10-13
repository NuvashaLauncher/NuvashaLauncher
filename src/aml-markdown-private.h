/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_MARKDOWN_PRIVATE_H
#define AML_MARKDOWN_PRIVATE_H

#include "aml-markdown.h"

char *aml_markdown_inline_allow_html (const char *text);
gboolean aml_markdown_url_is_safe (const char *url);

#endif
