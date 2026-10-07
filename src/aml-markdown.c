/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-markdown.h"

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

static void inline_into (GString *out, const char *text, gsize length);

/* Reads "label](url)" that follows a "[", and writes the markup for a
 * link into @out. @after is set to what comes after the closing ")".
 * FALSE when this is not a link, in which case nothing is written. */
static gboolean
inline_link (GString *out, const char *text, gsize length, const char **after)
{
  const char *close = memchr (text, ']', length);
  const char *paren = NULL;
  g_autofree char *label = NULL;
  g_autofree char *url = NULL;

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
      inline_into (out, label, strlen (label));
      g_string_append (out, "</a>");
    }
  else
    {
      /* A link that cannot be opened is still words. */
      inline_into (out, label, strlen (label));
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

/* The markup of @text, whose length is @length. Recurses for the words
 * inside emphasis and links. */
static void
inline_into (GString *out, const char *text, gsize length)
{
  gsize i = 0;

  while (i < length)
    {
      char c = text[i];

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

          if (inline_link (out, text + i + 2, length - i - 2, &after))
            {
              i = (after - text);
              continue;
            }
        }

      if (c == '[')
        {
          const char *after = NULL;

          if (inline_link (out, text + i + 1, length - i - 1, &after))
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
              inline_into (out, hay, end - hay);
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
              inline_into (out, hay, end - hay);
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
              inline_into (out, hay, end - hay);
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

  inline_into (out, text, strlen (text));

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

  /* An image with its alt text first, so the text beside it is kept. */
  step = regex_replace (
      out,
      "<img[^>]*src\\s*=\\s*[\"']([^\"']+)[\"'][^>]*alt\\s*=\\s*[\"']([^\"']*)[\"'][^>]*>",
      "![$2]($1)", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  step = regex_replace (out,
                        "<img[^>]*src\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>",
                        "![]($1)", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  step = regex_replace (out, "<a[^>]*href\\s*=\\s*[\"']([^\"']+)[\"'][^>]*>(.*?)</a>",
                        "[$2]($1)", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  step = regex_replace (out, "<(strong|b)[^>]*>(.*?)</\\1>", "**$2**",
                        G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  step = regex_replace (out, "<(em|i)[^>]*>(.*?)</\\1>", "*$2*",
                        G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  /* A list item becomes a line of its own with a marker. */
  step = regex_replace (out, "<li[^>]*>", "\n- ", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  step = regex_replace (out, "<br\\s*/?>", "\n", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  /* A paragraph is a gap in the words, whatever it is made of. */
  step = regex_replace (out, "</?p[^>]*>", "\n\n", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  step = regex_replace (out, "</?(ul|ol|dl|center|div|span|table|tr|td|th|thead|tbody|figure|figcaption)[^>]*>",
                        "\n", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  /* Closing tags that have nothing to open. */
  step = regex_replace (out, "</(li|ul|ol|dl|center|div|span|table|tr|td|th|thead|tbody|figure|figcaption)>",
                        "", G_REGEX_CASELESS | G_REGEX_DOTALL);
  g_free (out);
  out = g_steal_pointer (&step);

  /* Whatever other tag is left names nothing a reader needs. */
  step = regex_replace (out, "<[^>]+>", "", G_REGEX_DOTALL);
  g_free (out);

  return g_steal_pointer (&step);
}

static GtkWidget *
wrapped_label (const char *markup)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_markup (GTK_LABEL (label), markup != NULL ? markup : "");
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);

  return label;
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
  g_autofree char *markup = aml_markdown_inline (markdown);
  GtkWidget *label = wrapped_label (markup);

  if (css_class != NULL)
    gtk_widget_add_css_class (label, css_class);

  g_signal_connect (label, "activate-link", G_CALLBACK (on_activate_link),
                    NULL);

  return label;
}

/* The picture of an image, filled in when the file has been fetched. */
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

      gtk_picture_set_file (GTK_PICTURE (picture), file);
      gtk_widget_set_visible (picture, TRUE);
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
  GtkWidget *picture = gtk_picture_new ();
  g_autofree char *resolved = resolve_image_url (url);

  gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_hexpand (picture, TRUE);
  gtk_widget_set_halign (picture, GTK_ALIGN_FILL);
  gtk_widget_set_visible (picture, FALSE);

  if (resolved != NULL && url_is_safe (resolved))
    aml_icon_load_async (resolved, NULL, image_ready, g_object_ref (picture));

  return picture;
}

/* Reads "![alt](url)" filling the whole of @line. TRUE when the line is
 * one image, in which case @url is set to its url, which the caller
 * frees. */
static gboolean
line_is_image (const char *line, char **url)
{
  const char *close = NULL;
  const char *paren = NULL;
  g_autofree char *found = NULL;

  if (!g_str_has_prefix (line, "!["))
    return FALSE;

  close = strchr (line + 2, ']');

  if (close == NULL || close[1] != '(')
    return FALSE;

  paren = strchr (close + 2, ')');

  if (paren == NULL || paren[1] != '\0')
    return FALSE;

  found = g_strndup (close + 2, paren - (close + 2));

  {
    char *space = strpbrk (found, " \t");

    if (space != NULL)
      *space = '\0';
  }

  if (url != NULL)
    *url = g_steal_pointer (&found);

  return TRUE;
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

/* TRUE when @line begins a block that is not a paragraph line. */
static gboolean
starts_block (const char *line)
{
  const char *content = NULL;
  char *bullet = NULL;
  gboolean block =
    heading_level (line) > 0 || g_str_has_prefix (line, "```") ||
    g_str_has_prefix (line, ">") || is_rule (line) ||
    line_is_image (line, NULL);

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
          gtk_box_append (box, label);
          continue;
        }

      level = heading_level (line);

      if (level > 0)
        {
          g_autofree char *css =
            g_strdup_printf ("title-%u", MIN (level, 4));

          gtk_box_append (
              box, text_block (g_strstrip ((char *) line + level), css));
          i++;
          continue;
        }

      if (line_is_image (line, &image_url))
        {
          gtk_box_append (box, image_block (image_url));
          i++;
          continue;
        }

      if (list_item (line, &content, &bullet))
        {
          GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          GtkWidget *dot = gtk_label_new (bullet);
          GtkWidget *item = text_block (content, NULL);

          gtk_widget_set_valign (dot, GTK_ALIGN_START);
          gtk_widget_add_css_class (dot, "dim-label");
          gtk_widget_set_valign (item, GTK_ALIGN_START);
          gtk_widget_set_hexpand (item, TRUE);
          gtk_box_append (GTK_BOX (row), dot);
          gtk_box_append (GTK_BOX (row), item);
          gtk_box_append (box, row);
          i++;
          continue;
        }

      if (g_str_has_prefix (line, ">"))
        {
          GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          GtkWidget *bar = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
          GtkWidget *label =
            text_block (g_strstrip ((char *) line + 1), "dim-label");

          gtk_widget_set_hexpand (label, TRUE);
          gtk_box_append (GTK_BOX (row), bar);
          gtk_box_append (GTK_BOX (row), label);
          gtk_box_append (box, row);
          i++;
          continue;
        }

      if (is_rule (line))
        {
          gtk_box_append (box, gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
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
        gtk_box_append (box, text_block (paragraph->str, NULL));
      else
        i++;
    }
}
