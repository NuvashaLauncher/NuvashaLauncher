/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-markdown.h"
#include "aml-task.h"

#include "aml-icons.h"

#include <string.h>

/* ---------------------------------------------------------- *
 * Inline spans
 * ---------------------------------------------------------- */

/* TRUE when @url may be opened: a link a body carries has no business
 * being a "file://" or a "javascript:". */
static gboolean
url_is_safe (const char *url)
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

  if (url_is_safe (url))
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

      if (body_len >= 2 && (body[1] == 'x' || body[1] == 'X'))
        {
          g_autofree char *hex = g_strndup (body + 2, body_len - 2);

          codepoint = (gunichar) g_ascii_strtoull (hex, NULL, 16);
        }
      else
        {
          g_autofree char *dec = g_strndup (body + 1, body_len - 1);

          codepoint = (gunichar) g_ascii_strtoull (dec, NULL, 10);
        }

      if (codepoint == 0 || codepoint > 0x10FFFF ||
          !g_unichar_validate (codepoint))
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
aml_markdown_inline (const char *text)
{
  g_autoptr (GString) out = g_string_new (NULL);

  if (text == NULL)
    return NULL;

  inline_into (out, text, strlen (text), FALSE);

  return g_string_free (g_steal_pointer (&out), FALSE);
}

/* The same, with the body's own HTML tags passed through. */
static char *
markdown_inline_html (const char *text)
{
  g_autoptr (GString) out = g_string_new (NULL);

  if (text == NULL)
    return NULL;

  inline_into (out, text, strlen (text), TRUE);

  return g_string_free (g_steal_pointer (&out), FALSE);
}

/* ---------------------------------------------------------- *
 * Blocks
 * ---------------------------------------------------------- */

/* A Modrinth body is Markdown, but a lot of them also carry HTML:
 * tags for an image, a link or a bit of bold, and a <center> or a <p>
 * here and there. The HTML is rewritten into the Markdown the reader
 * below already knows, so one path renders both. Only the tags a body
 * actually uses are named; any other tag is dropped, keeping the words
 * around it. */
static char *
regex_replace (const char *text, const char *pattern, const char *replacement,
               GRegexCompileFlags flags)
{
  g_autoptr (GRegex) regex =
    g_regex_new (pattern, flags | G_REGEX_OPTIMIZE, 0, NULL);

  if (regex == NULL)
    return g_strdup (text);

  return g_regex_replace (regex, text, -1, 0, replacement, 0, NULL);
}

static char *
html_to_markdown (const char *text)
{
  g_autofree char *step = NULL;
  char *out = NULL;

  /* Comments carry no words, only a note to whoever edits the body. */
  step = regex_replace (text, "<!--.*?-->", "", G_REGEX_DOTALL);
  out = g_steal_pointer (&step);

#define REPLACE(pattern, replacement, flags)                                 \
  G_STMT_START                                                               \
  {                                                                          \
    step = regex_replace (out, pattern, replacement, flags);                 \
    g_free (out);                                                            \
    out = g_steal_pointer (&step);                                           \
  }                                                                          \
  G_STMT_END

  /* A block of code, kept verbatim. */
  REPLACE ("<pre[^>]*>(.*?)</pre>", "\n\n```\n\\1\n```\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* A video embed becomes a link that opens it. */
  REPLACE ("<iframe[^>]*src\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>\\s*</iframe>",
           "", G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<iframe[^>]*src\\s*=\\s*[\"']([^\"']+)[\"'][^>]*/?>",
           "", G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* An image with its alt text first, so the text beside it is kept.
     The attributes come in either order. */
  REPLACE (
      "<img[^>]*src\\s*=\\s*[\"']([^\"']+)[\"'][^>]*alt\\s*=\\s*[\"']([^\"']*)[\"'][^>]*>",
      "", G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE (
      "<img[^>]*alt\\s*=\\s*[\"']([^\"']*)[\"'][^>]*src\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>",
      "", G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<img[^>]*src\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>",
           "", G_REGEX_CASELESS | G_REGEX_DOTALL);

  REPLACE ("<a[^>]*href\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>(.*?)</a>",
           "[\\2](\\1)", G_REGEX_CASELESS | G_REGEX_DOTALL);

  REPLACE ("<(strong|b)[^>]*>(.*?)</\\1>", "**\\2**",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<(em|i)[^>]*>(.*?)</\\1>", "*\\2*",
           G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* Headings, before the block tags below turn them into gaps. */
  REPLACE ("<h1[^>]*>(.*?)</h1>", "\n\n# \\1\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<h2[^>]*>(.*?)</h2>", "\n\n## \\1\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<h3[^>]*>(.*?)</h3>", "\n\n### \\1\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<h4[^>]*>(.*?)</h4>", "\n\n#### \\1\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<h5[^>]*>(.*?)</h5>", "\n\n##### \\1\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<h6[^>]*>(.*?)</h6>", "\n\n###### \\1\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* A table becomes pipe rows the block reader folds into a grid: a
     header cell is bold, so a row of <th> reads as a header. */
  REPLACE ("<caption[^>]*>(.*?)</caption>", "\n\n**\\1**\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<th[^>]*>(.*?)</th>", "| **\\1** ",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<td[^>]*>(.*?)</td>", "| \\1 ",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("<tr[^>]*>", "\n", G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("</tr>", " |\n", G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("</?t(able|head|body|foot)[^>]*>", "\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* <summary> is the headline of what a <details> folds away; the fold
     itself is left open, so the words are what the reader sees. */
  REPLACE ("<summary[^>]*>(.*?)</summary>", "\n\n**\\1**\n\n",
           G_REGEX_CASELESS | G_REGEX_DOTALL);
  REPLACE ("</?details[^>]*>", "\n\n", G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* A list item becomes a line of its own with a marker. */
  REPLACE ("<li[^>]*>", "\n- ", G_REGEX_CASELESS | G_REGEX_DOTALL);

  REPLACE ("<br\\s*/?>", "\n", G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* A paragraph or a block is a gap in the words. */
  REPLACE (
      "</?(p|div|center|section|article|header|footer|main|blockquote|ul|ol|dl|dt|dd|figure|figcaption)[^>]*>",
      "\n\n", G_REGEX_CASELESS | G_REGEX_DOTALL);

  REPLACE ("<hr\\s*/?>", "\n\n---\n\n", G_REGEX_CASELESS | G_REGEX_DOTALL);

  /* Whatever other tag is left names nothing a reader needs. The tags
     the reader does render are left for the inline pass. */
  REPLACE ("<(?!\\/?(?:sub|sup|strong|strike|del|code|em|u|s|b|i)(?=[\\s>/]))[^>]*>",
           "", G_REGEX_CASELESS | G_REGEX_DOTALL);

#undef REPLACE

  return g_steal_pointer (&out);
}

static GtkWidget *
wrapped_label (const char *markup)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (NULL));
  g_autoptr (PangoAttrList) attrs = NULL;
  g_autofree char *plain = NULL;
  g_autoptr (GError) error = NULL;

  /* Modrinth descriptions are user content and occasionally contain HTML
     with mismatched tags. Never pass invalid markup to GtkLabel: it logs a
     warning and can leave the label with a broken size allocation. */
  if (markup == NULL ||
      pango_parse_markup (markup, -1, 0, &attrs, &plain, NULL, &error))
    gtk_label_set_markup (GTK_LABEL (GTK_WIDGET (label)), markup != NULL ? markup : "");
  else
    {
      g_autoptr (GRegex) tags =
        g_regex_new ("<[^>]*>", G_REGEX_DOTALL, 0, NULL);
      g_autofree char *fallback =
        g_regex_replace (tags, markup, -1, 0, "", 0, NULL);

      gtk_label_set_text (GTK_LABEL (GTK_WIDGET (label)), fallback != NULL ? fallback : "");
    }

  gtk_label_set_wrap (GTK_LABEL (GTK_WIDGET (label)), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (GTK_WIDGET (label)), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (GTK_WIDGET (label)), 0);
  gtk_label_set_selectable (GTK_LABEL (GTK_WIDGET (label)), TRUE);

  return GTK_WIDGET (label);
}

static gboolean
on_activate_link (GtkLabel *label, const char *uri,
                  gpointer user_data G_GNUC_UNUSED)
{
  g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (uri);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (label));

  gtk_uri_launcher_launch (launcher,
                           GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                           NULL, NULL, NULL);

  return TRUE;
}

/* A paragraph or a heading: one label whose links open. */
static GtkWidget *
text_block (const char *markdown, const char *css_class)
{
  g_autofree char *markup = markdown_inline_html (markdown);
  GtkWidget *label = wrapped_label (markup);

  if (markup == NULL || *markup == '\0')
    gtk_widget_set_visible (GTK_WIDGET (label), FALSE);

  if (css_class != NULL)
    gtk_widget_add_css_class (GTK_WIDGET (label), css_class);

  g_signal_connect (label, "activate-link", G_CALLBACK (on_activate_link),
                    NULL);

  return label;
}

/* The picture of an image, filled in when the file has been fetched. */
static void
image_resize (GtkWidget *picture)
{
  const char *ratio_text = g_object_get_data (
    G_OBJECT (picture), "aml-image-ratio");
  double ratio = ratio_text != NULL ? g_ascii_strtod (ratio_text, NULL) : 0.0;
  int width = gtk_widget_get_width (GTK_WIDGET (picture));

  if (ratio > 0.0 && width > 0)
    gtk_widget_set_size_request (GTK_WIDGET (picture), -1,
                                 MAX (1, (int) ((double) width / ratio + 0.5)));
}

static gboolean
image_resize_idle (gpointer user_data)
{
  GtkWidget *picture = user_data;
  AmlTaskScope *scope = g_object_get_data (G_OBJECT (picture),
                                           "aml-image-scope");
  guint source_id = GPOINTER_TO_UINT (
    g_object_get_data (G_OBJECT (picture), "aml-image-source"));

  if (scope != NULL)
    aml_task_scope_forget_source (scope, source_id);
  image_resize (picture);
  g_object_unref (picture);
  return G_SOURCE_REMOVE;
}

static void
image_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  GtkWidget *picture = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = aml_icon_load_finish (result, &error);

  if (path != NULL)
    {
      g_autoptr (GFile) file = g_file_new_for_path (path);
      g_autoptr (GdkTexture) texture = gdk_texture_new_from_file (file, NULL);

      /* A decoded texture, not the file: gtk_picture_set_file() hands
         some formats back as a symbolic paintable, and a symbolic
         picture laid out at a height of zero trips a Gtk critical while
         it is being drawn. A texture is never symbolic. */
      if (texture != NULL)
        {
          gtk_picture_set_paintable (GTK_PICTURE (GTK_WIDGET (picture)),
                                     GDK_PAINTABLE (texture));
          {
            int width = gdk_paintable_get_intrinsic_width (
              GDK_PAINTABLE (texture));
            int height = gdk_paintable_get_intrinsic_height (
              GDK_PAINTABLE (texture));

            if (width > 0 && height > 0)
              g_object_set_data_full (
                G_OBJECT (picture), "aml-image-ratio",
                g_strdup_printf ("%.17g", (double) width / (double) height),
                g_free);
          }
          gtk_widget_set_visible (GTK_WIDGET (picture), TRUE);
          gtk_widget_queue_resize (GTK_WIDGET (picture));
          AmlTaskScope *scope = g_object_get_data (
            G_OBJECT (picture), "aml-image-scope");
          if (scope == NULL)
            {
              scope = aml_task_scope_new (NULL, NULL, NULL, NULL);
              g_object_set_data_full (G_OBJECT (picture), "aml-image-scope",
                                      scope, (GDestroyNotify) aml_task_scope_free);
            }
          guint source_id = g_idle_add (image_resize_idle,
                                        g_object_ref (picture));
          g_object_set_data (G_OBJECT (picture), "aml-image-source",
                             GUINT_TO_POINTER (source_id));
          aml_task_scope_track_source (scope, source_id);
        }
    }

  g_object_unref (picture);
}

/* Modrinth writes an image with an absolute url, or one that starts at
 * the site root. */
static char *
resolve_image_url (const char *url)
{
  if (url == NULL || *url == '\0')
    return NULL;

  if (g_str_has_prefix (url, "//"))
    return g_strconcat ("https:", url, NULL);

  if (url[0] == '/')
    return g_strconcat ("https://cdn.modrinth.com", url, NULL);

  return g_strdup (url);
}

static GtkWidget *
image_block (const char *url)
{
  GtkPicture *picture = GTK_PICTURE (gtk_picture_new ());
  g_autofree char *resolved = resolve_image_url (url);

  /* The aspect frame supplies the correct height; the picture itself must
     be allowed to scale down to the description column's width. */
  gtk_picture_set_can_shrink (GTK_PICTURE (GTK_WIDGET (picture)), TRUE);
  gtk_picture_set_content_fit (GTK_PICTURE (GTK_WIDGET (picture)), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_hexpand (GTK_WIDGET (picture), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (picture), GTK_ALIGN_FILL);
  gtk_widget_set_visible (GTK_WIDGET (picture), FALSE);

  if (resolved != NULL && url_is_safe (resolved))
    aml_icon_load_async (resolved, NULL, image_ready, g_object_ref (picture));

  return GTK_WIDGET (picture);
}

/* Reads "![alt](url)" filling the whole of @line, with or without an
 * outer "[…](link)" around it. TRUE when the line is one image, in which
 * case @url is set to the image's url, which the caller frees. */
static gboolean
line_is_image (const char *line, char **url)
{
  /* Descriptions deliberately do not display remote images. */
  (void) line;
  (void) url;
  return FALSE;

}

static guint
heading_level (const char *line)
{
  guint level = 0;

  while (level < 6 && line[level] == '#')
    level++;

  if (level == 0 || (line[level] != ' ' && line[level] != '\0'))
    return 0;

  return level;
}

/* "- ", "* " or "1. " at the start, after indentation. @content is what
 * follows the marker, and @bullet is the marker a user sees. */
static gboolean
list_item (const char *line, const char **content, char **bullet)
{
  guint i = 0;
  guint digits = 0;

  while (line[i] == ' ' || line[i] == '\t')
    i++;

  if ((line[i] == '-' || line[i] == '*' || line[i] == '+') &&
      line[i + 1] == ' ')
    {
      *content = line + i + 2;

      if (bullet != NULL)
        *bullet = g_strdup ("•");

      return TRUE;
    }

  while (g_ascii_isdigit (line[i + digits]))
    digits++;

  if (digits > 0 && (line[i + digits] == '.' || line[i + digits] == ')') &&
      line[i + digits + 1] == ' ')
    {
      *content = line + i + digits + 2;

      if (bullet != NULL)
        *bullet = g_strndup (line + i, digits + 1);

      return TRUE;
    }

  return FALSE;
}

static gboolean
is_blank (const char *line)
{
  for (const char *p = line; *p != '\0'; p++)
    if (*p != ' ' && *p != '\t')
      return FALSE;

  return TRUE;
}

static gboolean
is_rule (const char *line)
{
  char c = '\0';
  guint count = 0;

  for (const char *p = line; *p != '\0'; p++)
    {
      if (*p == ' ')
        continue;

      if (*p != '-' && *p != '*' && *p != '_')
        return FALSE;

      if (c == '\0')
        c = *p;
      else if (*p != c)
        return FALSE;

      count++;
    }

  return count >= 3;
}

/* TRUE when @line is one row of a table written as pipe columns. */
static gboolean
is_table_row (const char *line)
{
  return line[0] == '|';
}

/* "|---|:--:|" and the like: the line GFM puts between a header and its
 * rows. It carries no words, so it is skipped. */
static gboolean
is_table_separator (const char *line)
{
  gboolean saw_dash = FALSE;

  if (line[0] != '|')
    return FALSE;

  for (const char *p = line; *p != '\0'; p++)
    {
      if (*p == '|' || *p == '-' || *p == ':' || *p == ' ' || *p == '\t')
        {
          if (*p == '-')
            saw_dash = TRUE;
          continue;
        }

      return FALSE;
    }

  return saw_dash;
}

/* The pipe rows of a table, laid out as a grid. */
static GtkWidget *
table_block (GPtrArray *rows)
{
  GtkGrid *grid = GTK_GRID (gtk_grid_new ());

  gtk_grid_set_row_spacing (GTK_GRID (GTK_WIDGET (grid)), 4);
  gtk_grid_set_column_spacing (GTK_GRID (GTK_WIDGET (grid)), 12);
  gtk_widget_set_hexpand (GTK_WIDGET (grid), TRUE);
  gtk_widget_set_margin_top (GTK_WIDGET (grid), 4);
  gtk_widget_set_margin_bottom (GTK_WIDGET (grid), 4);

  for (guint r = 0; r < rows->len; r++)
    {
      char **cells = g_ptr_array_index (rows, r);
      guint n = g_strv_length (cells);
      guint column = 0;

      for (guint c = 0; c < n; c++)
        {
          /* The empty pieces the split leaves at both ends. */
          if ((c == 0 || c + 1 == n) && cells[c][0] == '\0')
            continue;

          {
            g_autofree char *cell = g_strdup (cells[c]);
            const char *text = g_strstrip (cell);
            g_autofree char *image_url = NULL;
            GtkWidget *widget = NULL;

            /* A cell that is one image is the image, as Modrinth draws it;
               anything else is the words it holds. */
            if (line_is_image (text, &image_url))
              widget = image_block (image_url);
            else
              widget = text_block (text, NULL);

            gtk_widget_set_hexpand (widget, TRUE);
            gtk_widget_set_valign (widget, GTK_ALIGN_START);
            gtk_grid_attach (GTK_GRID (GTK_WIDGET (grid)), widget, column, r, 1, 1);
            column++;
          }
        }
    }

  return GTK_WIDGET (grid);
}

/* TRUE when @line begins a block that is not a paragraph line. */
static gboolean
starts_block (const char *line)
{
  const char *content = NULL;
  char *bullet = NULL;
  gboolean block =
    heading_level (line) > 0 || g_str_has_prefix (line, "```") ||
    g_str_has_prefix (line, ">") || is_rule (line) ||
    is_table_row (line) || line_is_image (line, NULL);

  if (!block && list_item (line, &content, &bullet))
    block = TRUE;

  g_free (bullet);
  return block;
}

void
aml_markdown_render (GtkBox *box, const char *markdown)
{
  g_auto (GStrv) lines = NULL;
  g_autofree char *text = NULL;
  guint i = 0;

  if (markdown == NULL)
    return;

  /* The HTML a body carries is turned into Markdown first, so both are
     read by the same blocks below. */
  text = html_to_markdown (markdown);
  lines = g_strsplit (text, "\n", -1);

  while (lines[i] != NULL)
    {
      g_autofree char *trimmed = g_strdup (lines[i]);
      const char *line = g_strstrip (trimmed);
      g_autoptr (GString) paragraph = g_string_new (NULL);
      guint level = 0;
      const char *content = NULL;
      g_autofree char *bullet = NULL;
      g_autofree char *image_url = NULL;

      if (is_blank (line))
        {
          i++;
          continue;
        }

      /* A fenced code block: every line up to the closing fence is one
         label, monospace and never read as markup. */
      if (g_str_has_prefix (line, "```"))
        {
          g_autoptr (GString) code = g_string_new (NULL);
          g_autofree char *escaped = NULL;
          g_autofree char *markup = NULL;
          GtkWidget *label = NULL;

          i++;

          while (lines[i] != NULL &&
                 !g_str_has_prefix (g_strstrip (lines[i]), "```"))
            {
              g_string_append (code, lines[i]);
              g_string_append_c (code, '\n');
              i++;
            }

          if (lines[i] != NULL)
            i++;

          escaped = g_markup_escape_text (code->str, -1);
          markup = g_strdup_printf (
              "<span font_family=\"monospace\">%s</span>", escaped);
          label = wrapped_label (markup);
          gtk_box_append (GTK_BOX (box), GTK_WIDGET (label));
          continue;
        }

      level = heading_level (line);

      if (level > 0)
        {
          g_autofree char *css =
            g_strdup_printf ("title-%u", MIN (level, 4));

          gtk_box_append (
              GTK_BOX (box), text_block (g_strstrip ((char *) line + level), css));
          i++;
          continue;
        }

      if (line_is_image (line, &image_url))
        {
          gtk_box_append (GTK_BOX (box), image_block (image_url));
          i++;
          continue;
        }

      if (is_table_row (line))
        {
          g_autoptr (GPtrArray) rows =
            g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);

          while (lines[i] != NULL)
            {
              g_autofree char *raw = g_strdup (lines[i]);
              const char *row = g_strstrip (raw);

              if (!is_table_row (row))
                break;

              if (!is_table_separator (row))
                g_ptr_array_add (rows, g_strsplit (row, "|", -1));

              i++;
            }

          if (rows->len > 0)
            gtk_box_append (GTK_BOX (box), table_block (rows));

          continue;
        }

      if (list_item (line, &content, &bullet))
        {
          GtkBox *row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
          GtkLabel *dot = GTK_LABEL (gtk_label_new (bullet));
          GtkWidget *item = text_block (content, NULL);

          gtk_widget_set_valign (GTK_WIDGET (dot), GTK_ALIGN_START);
          gtk_widget_add_css_class (GTK_WIDGET (dot), "dim-label");
          gtk_widget_set_valign (item, GTK_ALIGN_START);
          gtk_widget_set_hexpand (item, TRUE);
          gtk_box_append (GTK_BOX (GTK_WIDGET (row)), GTK_WIDGET (dot));
          gtk_box_append (GTK_BOX (GTK_WIDGET (row)), item);
          gtk_box_append (GTK_BOX (box), GTK_WIDGET (row));
          i++;
          continue;
        }

      if (g_str_has_prefix (line, ">"))
        {
          GtkBox *row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
          GtkSeparator *bar = GTK_SEPARATOR (gtk_separator_new (GTK_ORIENTATION_VERTICAL));
          GtkWidget *label =
            text_block (g_strstrip ((char *) line + 1), "dim-label");

          gtk_widget_set_hexpand (GTK_WIDGET (label), TRUE);
          gtk_box_append (GTK_BOX (GTK_WIDGET (row)), GTK_WIDGET (bar));
          gtk_box_append (GTK_BOX (GTK_WIDGET (row)), GTK_WIDGET (label));
          gtk_box_append (GTK_BOX (box), GTK_WIDGET (row));
          i++;
          continue;
        }

      if (is_rule (line))
        {
          gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
          i++;
          continue;
        }

      /* A paragraph: every line up to the next blank or block. */
      while (lines[i] != NULL)
        {
          g_autofree char *next = g_strdup (lines[i]);
          const char *current = g_strstrip (next);

          if (is_blank (current) || starts_block (current))
            break;

          if (paragraph->len > 0)
            g_string_append_c (paragraph, ' ');

          g_string_append (paragraph, current);
          i++;
        }

      if (paragraph->len > 0)
        gtk_box_append (GTK_BOX (box), text_block (paragraph->str, NULL));
      else
        i++;
    }
}
