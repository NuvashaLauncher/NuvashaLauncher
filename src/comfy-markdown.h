/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyMarkdown: the Markdown a Modrinth project writes its description in,
 * shown as widgets.
 *
 * Modrinth serves a project's long description as Markdown, with headings,
 * lists, links and images. A GtkLabel cannot hold an image, and Pango
 * markup is not Markdown, so the text is read here and one widget per
 * block is built: a label for a paragraph or a heading, a picture for an
 * image, a link that opens in the browser for a link.
 *
 * Only the parts Modrinth bodies use are understood. Anything else is
 * shown as the plain text it is rather than dropped. */

#ifndef comfy_MARKDOWN_H
#define comfy_MARKDOWN_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Renders @markdown into @box, a vertical box, appending one widget per
 * block. Images are fetched asynchronously and appear in place when they
 * arrive; a link opens in the browser the way a link does everywhere
 * else. */
void comfy_markdown_render (GtkBox *box, const char *markdown);

/* The Pango markup of one span of inline Markdown: emphasis, code spans
 * and links. The text is escaped, so what comes back is safe to hand to
 * gtk_label_set_markup(). NULL when @text is NULL. (transfer full) */
char *comfy_markdown_inline (const char *text);

G_END_DECLS

#endif /* comfy_MARKDOWN_H */
