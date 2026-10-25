/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The subset of Markdown a Modrinth body uses, turned into Pango markup.
 *
 * What matters here is that the text is escaped and the markup is ours:
 * a project description holds "&", "<" and words that look like tags,
 * and a label that reads them as markup would show the wrong thing. A
 * link is only a link when it can be opened. Everything here is local. */

#include <glib.h>

#include "nuvasha-markdown.h"

static void
check (const char *input, const char *expected)
{
  g_autofree char *markup = nuvasha_markdown_inline (input);

  g_assert_cmpstr (markup, ==, expected);
}

static void
test_escaped (void)
{
  check ("Fresh & <b>Smooth</b>",
         "Fresh &amp; &lt;b&gt;Smooth&lt;/b&gt;");
  g_print ("ok - markup characters in a body are plain text\n");
}

static void
test_emphasis (void)
{
  check ("**bold**", "<b>bold</b>");
  check ("*italic*", "<i>italic</i>");
  check ("~~gone~~", "<s>gone</s>");
  check ("`code`", "<span font_family=\"monospace\">code</span>");
  g_print ("ok - emphasis and code spans become markup\n");
}

static void
test_link (void)
{
  check ("[Modrinth](https://modrinth.com)",
         "<a href=\"https://modrinth.com\">Modrinth</a>");

  check ("[click](file:///etc/passwd)", "click");
  g_print ("ok - only a link that can be opened becomes one\n");
}

static void
test_null (void)
{
  g_assert_null (nuvasha_markdown_inline (NULL));
  g_print ("ok - no text is no markup\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Markdown\n");

  test_escaped ();
  test_emphasis ();
  test_link ();
  test_null ();

  return 0;
}
