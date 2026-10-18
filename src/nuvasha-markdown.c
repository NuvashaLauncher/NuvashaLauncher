/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-markdown.h"
#include "nuvasha-markdown-private.h"
#include "nuvasha-task.h"

#include "nuvasha-icons.h"

#include <string.h>

static char *
markdown_inline_html (const char *text)
{
  g_autoptr (GString) out = g_string_new (NULL);

  if (text == NULL)
    return NULL;

  g_autofree char *markup = nuvasha_markdown_inline_allow_html (text);
  g_string_append (out, markup != NULL ? markup : "");

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
    G_OBJECT (picture), "nuvasha-image-ratio");
  double ratio = ratio_text != NULL ? g_ascii_strtod (ratio_text, NULL) : 0.0;
  int width = gtk_widget_get_width (GTK_WIDGET (picture));

  if (ratio > 0.0 && width > 0)
    gtk_widget_set_size_request (GTK_WIDGET (picture), -1,
                                 MAX (1, (int) ((double) width / ratio + 0.5)));
}

static gboolean
image_resize_idle (gpointer user_data)
{
  GtkWidget *picture = GTK_WIDGET (user_data);
  NuvashaTaskScope *scope = g_object_get_data (G_OBJECT (picture),
                                           "nuvasha-image-scope");
  guint source_id = GPOINTER_TO_UINT (
    g_object_get_data (G_OBJECT (picture), "nuvasha-image-source"));

  if (scope != NULL)
    nuvasha_task_scope_forget_source (scope, source_id);
  image_resize (picture);
  g_object_unref (picture);
  return G_SOURCE_REMOVE;
}

/* What one image load keeps alive: the picture the texture goes in, and
 * the operation that has to be released when the answer arrives. */
typedef struct
{
  GtkWidget *picture;
  NuvashaTaskScope *operation;
} ImageLoad;

static void
image_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  ImageLoad *load = user_data;
  GtkWidget *picture = load->picture;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = nuvasha_icon_load_finish (result, &error);

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
                G_OBJECT (picture), "nuvasha-image-ratio",
                g_strdup_printf ("%.17g", (double) width / (double) height),
                g_free);
          }
          gtk_widget_set_visible (GTK_WIDGET (picture), TRUE);
          gtk_widget_queue_resize (GTK_WIDGET (picture));
          NuvashaTaskScope *scope = g_object_get_data (
            G_OBJECT (picture), "nuvasha-image-scope");
          if (scope == NULL)
            {
              scope = nuvasha_task_scope_new (NULL, NULL, NULL, NULL);
              g_object_set_data_full (G_OBJECT (picture), "nuvasha-image-scope",
                                      scope, (GDestroyNotify) nuvasha_task_scope_unref);
            }
          guint source_id = g_idle_add (image_resize_idle,
                                        g_object_ref (picture));
          g_object_set_data (G_OBJECT (picture), "nuvasha-image-source",
                             GUINT_TO_POINTER (source_id));
          nuvasha_task_scope_track_source (scope, source_id);
        }
    }

  g_clear_object (&load->picture);
  if (load->operation != NULL)
    nuvasha_task_scope_unref (load->operation);
  g_free (load);
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

  if (resolved != NULL && nuvasha_markdown_url_is_safe (resolved))
    {
      ImageLoad *load = g_new0 (ImageLoad, 1);

      load->picture = GTK_WIDGET (g_object_ref (picture));
      load->operation = nuvasha_icon_load_async (resolved, image_ready, load);
    }

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
nuvasha_markdown_render (GtkBox *box, const char *markdown)
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
