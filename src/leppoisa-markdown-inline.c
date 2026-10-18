/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Inline Markdown parsing and safe Pango markup generation. Block/widget
 * rendering remains in leppoisa-markdown.c. */

#include "leppoisa-markdown-private.h"

#include <string.h>

/* ---------------------------------------------------------- *
 * Inline spans
 * ---------------------------------------------------------- */

/* TRUE when @url may be opened: a link a body carries has no business
 * being a "file://" or a "javascript:". */
gboolean
leppoisa_markdown_url_is_safe (const char *url)
{
  return url != NULL &&
         (g_ascii_strncasecmp (url, "http://", 7) == 0 ||
          g_ascii_strncasecmp (url, "https://", 8) == 0 ||
          g_ascii_strncasecmp (url, "mailto:", 7) == 0);
}

static void inline_into (GString *out, const char *text, gsize length,
                         gboolean allow_html);

/* Reads "label](url)" that follows a "[", and writes the markup for a
 * link into @out. @after is set to what comes after the closing ")".
 * FALSE when this is not a link, in which case nothing is written. */
static gboolean
inline_link (GString *out, const char *text, gsize length, gboolean allow_html,
             const char **after)
{
  const char *close = NULL;
  const char *paren = NULL;
  g_autofree char *label = NULL;
  g_autofree char *url = NULL;
  gsize depth = 0;

  /* The label may hold a nested "![…](…)" of its own, so the "]" that
     closes it is the one at depth zero, not the first one there is. */
  for (const char *p = text; p < text + length; p++)
    {
      if (*p == '[')
        depth++;
      else if (*p == ']')
        {
          if (depth == 0)
            {
              close = p;
              break;
            }

          depth--;
        }
    }

  if (close == NULL || close + 1 >= text + length || close[1] != '(')
    return FALSE;

  paren = memchr (close + 2, ')', (text + length) - (close + 2));

  if (paren == NULL)
    return FALSE;

  label = g_strndup (text, close - text);
  url = g_strndup (close + 2, paren - (close + 2));

  /* A title may follow the url after whitespace; only the url is used. */
  {
    char *space = strpbrk (url, " \t\n");

    if (space != NULL)
      *space = '\0';
  }

  if (leppoisa_markdown_url_is_safe (url))
    {
      g_autofree char *escaped = g_markup_escape_text (url, -1);

      g_string_append_printf (out, "<a href=\"%s\">", escaped);

      /* Images are intentionally hidden in descriptions, including an
         image wrapped in a link. */
      if (g_str_has_prefix (label, "!["))
        {
          /* Keep consuming the outer link, but emit no text or markup. */
        }
      else
        inline_into (out, label, strlen (label), allow_html);

      g_string_append (out, "</a>");
    }
  else
    {
      /* A link that cannot be opened is still words. */
      inline_into (out, label, strlen (label), allow_html);
    }

  *after = paren + 1;
  return TRUE;
}

/* One char of plain text, escaped for Pango markup. */
static void
inline_char (GString *out, char c)
{
  switch (c)
    {
    case '&':
      g_string_append (out, "&amp;");
      break;
    case '<':
      g_string_append (out, "&lt;");
      break;
    case '>':
      g_string_append (out, "&gt;");
      break;
    default:
      g_string_append_c (out, c);
      break;
    }
}

/* The characters a body actually names with an entity. */
static const struct {
  const char *name;
  const char *utf8;
} entities[] = {
  { "amp", "&" },        { "lt", "<" },          { "gt", ">" },
  { "quot", "\"" },      { "apos", "'" },        { "nbsp", "\xc2\xa0" },
  { "mdash", "\xe2\x80\x94" }, { "ndash", "\xe2\x80\x93" },
  { "hellip", "\xe2\x80\xa6" }, { "copy", "\xc2\xa9" },
  { "reg", "\xc2\xae" }, { "trade", "\xe2\x84\xa2" },
  { "laquo", "\xc2\xab" }, { "raquo", "\xc2\xbb" },
  { "middot", "\xc2\xb7" }, { "bull", "\xe2\x80\xa2" },
  { "times", "\xc3\x97" }, { "divide", "\xc3\xb7" },
  { "deg", "\xc2\xb0" }, { "plusmn", "\xc2\xb1" },
  { "micro", "\xc2\xb5" }, { "para", "\xc2\xb6" }, { "sect", "\xc2\xa7" },
  { "euro", "\xe2\x82\xac" }, { "pound", "\xc2\xa3" },
  { "yen", "\xc2\xa5" }, { "cent", "\xc2\xa2" },
  { "larr", "\xe2\x86\x90" }, { "rarr", "\xe2\x86\x92" },
  { "uarr", "\xe2\x86\x91" }, { "darr", "\xe2\x86\x93" },
  { "harr", "\xe2\x86\x94" }, { "lsquo", "\xe2\x80\x98" },
  { "rsquo", "\xe2\x80\x99" }, { "ldquo", "\xe2\x80\x9c" },
  { "rdquo", "\xe2\x80\x9d" }, { "star", "\xe2\x98\x85" },
  { "check", "\xe2\x9c\x94" }, { "cross", "\xe2\x9c\x97" },
};

static void
append_escaped (GString *out, const char *utf8)
{
  g_autofree char *escaped = g_markup_escape_text (utf8, -1);

  g_string_append (out, escaped);
}

/* "&amp;" or "&#x27;" into the character it names, escaped for Pango.
 * FALSE when this is not an entity, so a bare "&" stays a "&". */
static gboolean
entity_into (GString *out, const char *text, gsize length, gsize *consumed)
{
  const char *semi = NULL;
  gsize n = 0;
  const char *body = NULL;
  gsize body_len = 0;

  if (length < 3 || text[0] != '&')
    return FALSE;

  /* No entity is longer than "&hellip;"; a ";" past that is not one. */
  semi = memchr (text, ';', MIN (length, 12));

  if (semi == NULL)
    return FALSE;

  n = (semi - text) + 1;
  body = text + 1;
  body_len = n - 2;

  if (body_len == 0)
    return FALSE;

  if (body[0] == '#')
    {
      gunichar codepoint = 0;
      guint64 parsed = 0;
      char *end = NULL;

      if (body_len >= 2 && (body[1] == 'x' || body[1] == 'X'))
        {
          g_autofree char *hex = g_strndup (body + 2, body_len - 2);

          parsed = g_ascii_strtoull (hex, &end, 16);
        }
      else
        {
          g_autofree char *dec = g_strndup (body + 1, body_len - 1);

          parsed = g_ascii_strtoull (dec, &end, 10);
        }

      if (end == NULL || *end != '\0' || parsed == 0 || parsed > 0x10FFFF)
        return FALSE;

      codepoint = (gunichar) parsed;

      if (!g_unichar_validate (codepoint))
        return FALSE;

      {
        char buf[8];
        int written = g_unichar_to_utf8 (codepoint, buf);

        g_autofree char *decoded = g_strndup (buf, written);

        append_escaped (out, decoded);
      }

      *consumed = n;
      return TRUE;
    }

  for (guint i = 0; i < G_N_ELEMENTS (entities); i++)
    {
      if (strlen (entities[i].name) == body_len &&
          g_ascii_strncasecmp (entities[i].name, body, body_len) == 0)
        {
          append_escaped (out, entities[i].utf8);
          *consumed = n;
          return TRUE;
        }
    }

  return FALSE;
}

/* The tags a Modrinth body sprinkles into its Markdown, as the Pango
 * markup that says the same thing. They are all simple pairs, so there
 * is no attribute to read and nothing to keep track of. */
static const struct {
  const char *html;
  const char *pango;
} inline_tags[] = {
  { "b", "b" },       { "strong", "b" }, { "i", "i" },
  { "em", "i" },      { "u", "u" },      { "s", "s" },
  { "strike", "s" },  { "del", "s" },    { "sub", "sub" },
  { "sup", "sup" },   { "code", "span" },
};

/* "<u>", "</sub>" … turned into Pango markup. @text starts at the "<".
 * FALSE when this is not a tag the reader renders (which leaves a "<"
 * that is only a character). */
static gboolean
inline_html_into (GString *out, const char *text, gsize length, gsize *consumed)
{
  const char *gt = NULL;
  g_autofree char *inner = NULL;
  gboolean closing = FALSE;
  char *name = NULL;
  gsize namelen = 0;

  if (length < 3 || text[0] != '<')
    return FALSE;

  if (!(g_ascii_isalpha (text[1]) || text[1] == '/'))
    return FALSE;

  gt = memchr (text, '>', length);

  if (gt == NULL || gt == text + 1)
    return FALSE;

  inner = g_strndup (text + 1, gt - text - 1);
  closing = inner[0] == '/';
  name = g_strstrip (inner + (closing ? 1 : 0));
  namelen = strcspn (name, " \t\r\n/");
  name[namelen] = '\0';

  for (guint i = 0; i < G_N_ELEMENTS (inline_tags); i++)
    {
      if (g_ascii_strcasecmp (name, inline_tags[i].html) != 0)
        continue;

      if (g_strcmp0 (inline_tags[i].pango, "span") == 0)
        {
          if (closing)
            g_string_append (out, "</span>");
          else
            g_string_append (out,
                             "<span font_family=\"monospace\">");
        }
      else
        g_string_append_printf (out, "<%s%s>", closing ? "/" : "",
                                inline_tags[i].pango);

      *consumed = (gt - text) + 1;
      return TRUE;
    }

  return FALSE;
}

/* The markup of @text, whose length is @length. Recurses for the words
 * inside emphasis and links. @allow_html lets the tags a body carries
 * through as Pango markup; leaving it FALSE escapes them, which is what
 * an inline fragment taken out of its body wants. */
static void
inline_into (GString *out, const char *text, gsize length, gboolean allow_html)
{
  gsize i = 0;

  while (i < length)
    {
      char c = text[i];

      if (allow_html && c == '<')
        {
          gsize used = 0;

          if (inline_html_into (out, text + i, length - i, &used))
            {
              i += used;
              continue;
            }
        }

      if (c == '&')
        {
          gsize used = 0;

          if (entity_into (out, text + i, length - i, &used))
            {
              i += used;
              continue;
            }
        }

      /* A code span: everything up to the next backtick, verbatim. */
      if (c == '`')
        {
          const char *end = memchr (text + i + 1, '`', length - i - 1);

          if (end != NULL)
            {
              g_autofree char *code =
                g_markup_escape_text (text + i + 1, end - (text + i + 1));

              g_string_append_printf (
                  out, "<span font_family=\"monospace\">%s</span>", code);
              i = (end - text) + 1;
              continue;
            }
        }

      /* An image is words until it can be drawn; as an inline span its
         alt text is what there is. */
      if (c == '!' && i + 1 < length && text[i + 1] == '[')
        {
          const char *after = NULL;
          g_autoptr (GString) hidden = g_string_new (NULL);

          if (inline_link (hidden, text + i + 2, length - i - 2, allow_html,
                           &after))
            {
              i = (after - text);
              continue;
            }
        }

      if (c == '[')
        {
          const char *after = NULL;

          if (inline_link (out, text + i + 1, length - i - 1, allow_html,
                           &after))
            {
              i = (after - text);
              continue;
            }
        }

      /* Bold, before the one-char emphasis it starts with. */
      if (i + 1 < length && c == text[i + 1] && (c == '*' || c == '_'))
        {
          const char *hay = text + i + 2;
          gsize hay_len = length - i - 2;
          const char *end = NULL;

          for (gsize j = 0; j + 1 < hay_len; j++)
            if (hay[j] == c && hay[j + 1] == c)
              {
                end = hay + j;
                break;
              }

          if (end != NULL && end > hay)
            {
              g_string_append (out, "<b>");
              inline_into (out, hay, end - hay, allow_html);
              g_string_append (out, "</b>");
              i = (end - text) + 2;
              continue;
            }
        }

      /* Strikethrough. */
      if (i + 1 < length && c == '~' && text[i + 1] == '~')
        {
          const char *hay = text + i + 2;
          gsize hay_len = length - i - 2;
          const char *end = memchr (hay, '~', hay_len);

          if (end != NULL && end + 1 < text + length && end[1] == '~' &&
              end > hay)
            {
              g_string_append (out, "<s>");
              inline_into (out, hay, end - hay, allow_html);
              g_string_append (out, "</s>");
              i = (end - text) + 2;
              continue;
            }
        }

      /* Italic: a single delimiter, only when it holds words. */
      if ((c == '*' || c == '_') && i + 1 < length && text[i + 1] != ' ' &&
          text[i + 1] != c)
        {
          const char *hay = text + i + 1;
          gsize hay_len = length - i - 1;
          const char *end = NULL;

          for (gsize j = 1; j < hay_len; j++)
            if (hay[j] == c && hay[j - 1] != ' ')
              {
                end = hay + j;
                break;
              }

          if (end != NULL && end > hay)
            {
              g_string_append (out, "<i>");
              inline_into (out, hay, end - hay, allow_html);
              g_string_append (out, "</i>");
              i = (end - text) + 1;
              continue;
            }
        }

      inline_char (out, c);
      i++;
    }
}

char *
leppoisa_markdown_inline (const char *text)
{
  g_autoptr (GString) out = g_string_new (NULL);

  if (text == NULL)
    return NULL;

  inline_into (out, text, strlen (text), FALSE);

  return g_string_free (g_steal_pointer (&out), FALSE);
}

char *
leppoisa_markdown_inline_allow_html (const char *text)
{
  g_autoptr (GString) out = g_string_new (NULL);

  if (text == NULL)
    return NULL;

  inline_into (out, text, strlen (text), TRUE);
  return g_string_free (g_steal_pointer (&out), FALSE);
}

/* The same, with the body's own HTML tags passed through. */
