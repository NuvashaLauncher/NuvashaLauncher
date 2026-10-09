/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Every string the launcher hands to gettext has to be in every catalog,
 * and every translation has to take the same arguments as the string it
 * translates. A msgid no catalog knows shows up in the interface as the
 * English it was written in, which is how a launcher ends up half
 * translated without anybody noticing; a translation that loses a %s is
 * worse, because the message that needed it reads as nothing at all.
 *
 * The msgids are read out of the sources rather than out of a POT file,
 * so there is nothing to keep in step: the source is what gettext is
 * handed.
 *
 * Reads the tree and nothing else, and opens no display. */

#include <glib.h>

#include <stdlib.h>
#include <string.h>

#ifndef AML_SRC_DIR
#define AML_SRC_DIR "../src"
#endif

#ifndef AML_PO_DIR
#define AML_PO_DIR "../po"
#endif

/* ---------------------------------------------------------- *
 * Reading C and .po string literals
 * ---------------------------------------------------------- */

/* The text between the quotes of the literal @open starts, with the
 * backslashes still in it. Returns a pointer past the closing quote, or
 * NULL when the run ends inside a literal. */
static const char *
literal_end (const char *open)
{
  const char *c = open + 1;

  while (*c != '\0' && *c != '"')
    {
      if (*c == '\\' && c[1] != '\0')
        c++;

      c++;
    }

  return (*c == '"') ? c + 1 : NULL;
}

/* What a run of escaped characters stands for. Only the escapes the
 * sources and the catalogs use are handled; anything else is left alone
 * rather than guessed at, so a difference surfaces as a missing entry
 * instead of a silent mismatch. */
static char *
unescape (const char *escaped)
{
  GString *out = g_string_new (NULL);

  for (const char *c = escaped; *c != '\0'; c++)
    {
      if (*c != '\\' || c[1] == '\0')
        {
          g_string_append_c (out, *c);
          continue;
        }

      c++;

      switch (*c)
        {
        case 'n': g_string_append_c (out, '\n'); break;
        case 't': g_string_append_c (out, '\t'); break;
        case 'r': g_string_append_c (out, '\r'); break;
        case 'a': g_string_append_c (out, '\a'); break;
        case 'b': g_string_append_c (out, '\b'); break;
        case 'f': g_string_append_c (out, '\f'); break;
        case 'v': g_string_append_c (out, '\v'); break;
        default: g_string_append_c (out, *c); break;
        }
    }

  return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------- *
 * The msgids in the source
 * ---------------------------------------------------------- */

/* The escaped text of the run of adjacent literals @open starts, up to
 * wherever that run ends. @after is left past the last quote of it.
 * Returns NULL when a quote never closes. */
static char *
literal_run (const char  *open,
             const char **after)
{
  g_autoptr (GString) out = g_string_new (NULL);
  const char *end = literal_end (open);

  while (end != NULL)
    {
      const char *next = end;

      g_string_append_len (out, open + 1, (gsize) (end - open - 2));

      while (g_ascii_isspace (*next))
        next++;

      if (*next != '"')
        break;

      open = next;
      end = literal_end (open);
    }

  if (end == NULL)
    return NULL;

  *after = end;
  return g_string_free (g_steal_pointer (&out), FALSE);
}

/* Collects the msgid of every _("...") in @text into @out. A run of
 * adjacent literals is one msgid, the way the compiler reads it, so a
 * sentence broken over several lines is checked as one string. */
static void
scan_text (const char *text,
           GHashTable *out)
{
  const char *cursor = text;

  while ((cursor = strstr (cursor, "_(")) != NULL)
    {
      const char *open = cursor + 2;
      g_autofree char *escaped = NULL;
      g_autofree char *msgid = NULL;

      cursor = open;

      while (g_ascii_isspace (*open))
        open++;

      if (*open != '"')
        continue;

      escaped = literal_run (open, &cursor);

      /* A quote that never closes means the source does not compile. */
      if (escaped == NULL)
        continue;

      msgid = unescape (escaped);

      if (*msgid != '\0')
        g_hash_table_add (out, g_steal_pointer (&msgid));
    }
}

static void
scan_file (const char *path,
           GHashTable *out)
{
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;

  if (!g_file_get_contents (path, &text, NULL, &error))
    g_error ("cannot read \"%s\": %s", path, error->message);

  scan_text (text, out);
}

/* ---------------------------------------------------------- *
 * The catalogs
 * ---------------------------------------------------------- */

typedef struct
{
  char *name;
  GHashTable *translations; /* msgid -> msgstr */
} Catalog;

/* The value of the msgid or msgstr @at names: the literal on that line,
 * plus every line below it that is nothing but a literal. @at ends past
 * the last of them. */
static char *
entry_value (GPtrArray *lines,
             guint      *at)
{
  g_autoptr (GString) out = g_string_new (NULL);
  const char *line = g_ptr_array_index (lines, *at);
  const char *open = strchr (line, '"');

  g_assert_nonnull (open);

  for (;;)
    {
      const char *end = literal_end (open);

      g_assert_nonnull (end);
      g_string_append_len (out, open + 1, (gsize) (end - open - 2));
      (*at)++;

      if (*at >= lines->len)
        break;

      line = g_ptr_array_index (lines, *at);

      if (line[0] != '"')
        break;

      open = line;
    }

  return unescape (out->str);
}

static Catalog *
read_catalog (const char *path)
{
  g_autofree char *contents = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) raw = NULL;
  g_autoptr (GPtrArray) lines = NULL;
  Catalog *catalog = g_new0 (Catalog, 1);
  guint at = 0;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("cannot read \"%s\": %s", path, error->message);

  catalog->name = g_path_get_basename (path);
  catalog->translations =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  raw = g_strsplit (contents, "\n", -1);
  lines = g_ptr_array_new ();

  for (guint i = 0; raw[i] != NULL; i++)
    if (*raw[i] != '\0')
      g_ptr_array_add (lines, raw[i]);

  while (at < lines->len)
    {
      const char *line = g_ptr_array_index (lines, at);
      g_autofree char *msgid = NULL;
      g_autofree char *msgstr = NULL;

      if (!g_str_has_prefix (line, "msgid "))
        {
          at++;
          continue;
        }

      msgid = entry_value (lines, &at);

      if (at < lines->len &&
          g_str_has_prefix (g_ptr_array_index (lines, at), "msgstr "))
        msgstr = entry_value (lines, &at);

      /* The header is an empty msgid holding the catalog's own settings,
         not a string the launcher shows. */
      if (msgstr == NULL || *msgid == '\0')
        continue;

      g_hash_table_insert (catalog->translations,
                           g_steal_pointer (&msgid),
                           g_steal_pointer (&msgstr));
    }

  return catalog;
}

static void
catalog_free (gpointer data)
{
  Catalog *catalog = data;

  g_clear_pointer (&catalog->name, g_free);
  g_clear_pointer (&catalog->translations, g_hash_table_unref);
  g_free (catalog);
}

/* ---------------------------------------------------------- *
 * The check
 * ---------------------------------------------------------- */

/* The printf conversions of a string, in order, one character each so
 * that a %s standing where the source had a %d is a difference and not
 * just two one-letter words. */
static GPtrArray *
conversions (const char *text)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);

  for (const char *c = text; *c != '\0'; c++)
    {
      g_autofree char *conversion = NULL;

      if (*c != '%')
        continue;

      c++;

      if (*c == '%')
        continue;

      while (strchr ("-+ #0", *c) != NULL && *c != '\0')
        c++;

      while (g_ascii_isdigit (*c) || *c == '*')
        c++;

      if (*c == '.')
        {
          c++;
          while (g_ascii_isdigit (*c) || *c == '*')
            c++;
        }

      while (strchr ("hlLqjzt", *c) != NULL && *c != '\0')
        c++;

      if (*c == '\0')
        break;

      conversion = g_strdup_printf ("%c", *c);
      g_ptr_array_add (out, g_steal_pointer (&conversion));
    }

  return out;
}

static gboolean
same_conversions (const char *msgid,
                  const char *msgstr)
{
  g_autoptr (GPtrArray) left = conversions (msgid);
  g_autoptr (GPtrArray) right = conversions (msgstr);

  if (left->len != right->len)
    return FALSE;

  for (guint i = 0; i < left->len; i++)
    if (g_strcmp0 (g_ptr_array_index (left, i),
                   g_ptr_array_index (right, i)) != 0)
      return FALSE;

  return TRUE;
}

static gint
compare_names (gconstpointer a,
               gconstpointer b)
{
  return g_strcmp0 (*(const char *const *) a, *(const char *const *) b);
}

static GPtrArray *
catalog_paths (void)
{
  g_autoptr (GDir) directory = g_dir_open (AML_PO_DIR, 0, NULL);
  GPtrArray *names;
  const char *entry;

  g_assert_nonnull (directory);

  names = g_ptr_array_new_with_free_func (g_free);

  while ((entry = g_dir_read_name (directory)) != NULL)
    if (g_str_has_suffix (entry, ".po"))
      g_ptr_array_add (names, g_strdup (entry));

  g_ptr_array_sort (names, compare_names);

  return names;
}

int
main (void)
{
  g_autoptr (GHashTable) used = NULL;
  g_autoptr (GPtrArray) paths = NULL;
  g_autoptr (GPtrArray) catalogs = NULL;
  guint missing = 0;
  guint broken = 0;
  guint leftover = 0;

  g_print ("# Translation catalogs\n");

  /* Every msgid the source hands to gettext. */
  used = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  {
    g_autoptr (GDir) sources = g_dir_open (AML_SRC_DIR, 0, NULL);
    const char *entry;

    g_assert_nonnull (sources);

    while ((entry = g_dir_read_name (sources)) != NULL)
      {
        g_autofree char *path = NULL;

        if (!g_str_has_suffix (entry, ".c"))
          continue;

        path = g_build_filename (AML_SRC_DIR, entry, NULL);
        scan_file (path, used);
      }
  }

  {
    guint n = g_hash_table_size (used);

    g_print ("found %u translated string(s) in %s\n", n, AML_SRC_DIR);
    g_assert_cmpuint (n, >, 0);
  }

  paths = catalog_paths ();
  g_assert_cmpuint (paths->len, >, 0);

  catalogs = g_ptr_array_new_with_free_func (catalog_free);

  for (guint i = 0; i < paths->len; i++)
    {
      g_autofree char *path =
        g_build_filename (AML_PO_DIR, g_ptr_array_index (paths, i), NULL);

      g_ptr_array_add (catalogs, read_catalog (path));
    }

  for (guint i = 0; i < catalogs->len; i++)
    {
      const Catalog *catalog = g_ptr_array_index (catalogs, i);
      GHashTableIter iter;
      gpointer key = NULL;
      gpointer value = NULL;

      g_hash_table_iter_init (&iter, used);

      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          const char *msgstr = g_hash_table_lookup (catalog->translations,
                                                    key);

          if (msgstr == NULL || *msgstr == '\0')
            {
              g_printerr ("%s: \"%s\" is not translated\n", catalog->name,
                          (const char *) key);
              missing++;
              continue;
            }

          if (!same_conversions ((const char *) key, msgstr))
            {
              g_printerr ("%s: \"%s\" is translated as \"%s\", which does "
                          "not take the same arguments\n",
                          catalog->name, (const char *) key, msgstr);
              broken++;
            }
        }

      /* What a catalog still holds and the source no longer has. Not a
         failure on its own: the language names reach gettext through a
         table rather than through a literal, so they read as unused
         here while being used. */
      g_hash_table_iter_init (&iter, catalog->translations);

      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          if (g_hash_table_contains (used, key))
            continue;

          g_print ("%s: \"%s\" is not used any more\n", catalog->name,
                   (const char *) key);
          leftover++;
        }
    }

  g_assert_cmpuint (missing, ==, 0);
  g_assert_cmpuint (broken, ==, 0);

  g_print ("ok - every string is translated in every catalog%s\n",
           leftover > 0 ? " (what each catalog has left over is named)"
                        : "");

  return EXIT_SUCCESS;
}