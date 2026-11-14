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

#include "nuvashalauncher-markdown-private.h"

static void
check (const char *input, const char *expected)
{
  g_autofree char *markup = nuvashalauncher_markdown_inline (input);

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
  check ("_italic_", "<i>italic</i>");
  check ("my_mod_id", "my_mod_id");
  check ("~~gone~~", "<s>gone</s>");
  check ("`code`", "<span font_family=\"monospace\">code</span>");
  check ("\\*not italic\\* and \\[not link\\](https://example.com)",
         "*not italic* and [not link](https://example.com)");
  g_print ("ok - emphasis, escapes and code spans become markup\n");
}

static void
test_link (void)
{
  check ("[Modrinth](https://modrinth.com)",
         "<a href=\"https://modrinth.com\">Modrinth</a>");

  check ("[click](file:///etc/passwd)", "click");
  check ("[![badge](https://img.example/a.png)](https://example.com)", "");
  g_print ("ok - only a link that can be opened becomes one\n");
}

static void
test_link_nested_parens (void)
{
  check ("[Wiki](https://en.wikipedia.org/wiki/Minecraft_(video_game))",
         "<a href=\"https://en.wikipedia.org/wiki/Minecraft_(video_game)\">Wiki</a>");
  g_print ("ok - links with nested parentheses are parsed correctly\n");
}

static void
test_null (void)
{
  g_assert_null (nuvashalauncher_markdown_inline (NULL));
  g_print ("ok - no text is no markup\n");
}

static void
test_html_entities_and_tags (void)
{
  g_autofree char *entities = nuvashalauncher_markdown_inline ("&quot;hello &amp; world&quot; &copy; &#x20ac; &#65;");
  g_assert_cmpstr (entities, ==, "&quot;hello &amp; world&quot; © € A");

  /* Test allow_html */
  g_autofree char *html = nuvashalauncher_markdown_inline_allow_html (
    "<b>bold</b> and <i>italic</i> and <code>code</code> and <sub>sub</sub> and <sup>sup</sup> and <del>del</del>");
  g_assert_cmpstr (html, ==,
    "<b>bold</b> and <i>italic</i> and <span font_family=\"monospace\">code</span> and <sub>sub</sub> and <sup>sup</sup> and <s>del</s>");

  /* Test safe URL checking */
  g_assert_true (nuvashalauncher_markdown_url_is_safe ("https://modrinth.com"));
  g_assert_true (nuvashalauncher_markdown_url_is_safe ("http://example.com"));
  g_assert_true (nuvashalauncher_markdown_url_is_safe ("mailto:test@example.com"));
  g_assert_false (nuvashalauncher_markdown_url_is_safe ("file:///etc/passwd"));
  g_assert_false (nuvashalauncher_markdown_url_is_safe ("javascript:alert(1)"));
  g_assert_false (nuvashalauncher_markdown_url_is_safe (NULL));
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
  test_link_nested_parens ();
  test_null ();
  test_html_entities_and_tags ();

  return 0;
}
