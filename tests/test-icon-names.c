/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Checks every icon name AML writes in its source against the icon theme
 * that is actually installed.
 *
 * An icon name that does not exist is not an error at build time and not
 * an error at run time either: GTK quietly draws nothing, so the button
 * is there and blank. The only place it can be caught is here.
 *
 * Needs a display to ask the theme, and skips itself when there is none,
 * so it never turns a headless run into a failure. */

#include <gtk/gtk.h>

#include <stdlib.h>
#include <string.h>

/* Every string literal in the sources that looks like an icon name. The
 * names go into a set, so the same name written in two files is checked
 * once. */
static void
scan_file (const char *path, GHashTable *names)
{
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;
  gsize length = 0;
  const char *p = NULL;

  if (!g_file_get_contents (path, &text, &length, &error))
    {
      g_printerr ("cannot read \"%s\": %s\n", path, error->message);
      g_clear_error (&error);
      return;
    }

  p = text;

  while ((p = strchr (p, '"')) != NULL)
    {
      const char *start = p + 1;
      const char *end = strchr (start, '"');
      gsize n = 0;

      if (end == NULL)
        break;

      n = (gsize) (end - start);

      /* The suffix is compared against the span and not with
         g_str_has_suffix(), which would look past the closing quote and
         into the rest of the file.
         A path is not an icon, however it ends. */
      if (n > 9 && memcmp (end - 9, "-symbolic", 9) == 0 &&
          memchr (start, '/', n) == NULL)
        {
          char *name = g_strndup (start, n);

          g_hash_table_add (names, name);
        }

      p = end;
    }
}

static gint
by_name (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(const char **) a, *(const char **) b);
}

int
main (int argc, char **argv)
{
  g_autoptr (GPtrArray) sorted = NULL;
  g_autoptr (GHashTable) names = NULL;
  GtkIconTheme *theme = NULL;
  GDir *dir = NULL;
  const char *entry = NULL;
  guint missing = 0;

  (void) argc;
  (void) argv;

  g_print ("# Icon name audit\n");

  names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  sorted = g_ptr_array_new ();

  dir = g_dir_open (AML_SRC_DIR, 0, NULL);
  g_assert_nonnull (dir);

  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *path = NULL;

      if (!g_str_has_suffix (entry, ".c"))
        continue;

      path = g_build_filename (AML_SRC_DIR, entry, NULL);
      scan_file (path, names);
    }

  g_dir_close (dir);

  {
    GHashTableIter iter;
    gpointer key = NULL;

    g_hash_table_iter_init (&iter, names);

    while (g_hash_table_iter_next (&iter, &key, NULL))
      g_ptr_array_add (sorted, key);
  }

  g_ptr_array_sort (sorted, by_name);

  g_print ("found %u icon name(s) in %s\n", sorted->len, AML_SRC_DIR);

  /* Asking the theme needs a display. Without one there is nothing to
     ask and nothing to fail: the check is skipped rather than faked. */
  if (!gtk_init_check ())
    {
      g_print ("skipped - no display, the icon theme cannot be opened\n");
      return 77;
    }

  theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());

  if (theme == NULL)
    {
      g_print ("skipped - no icon theme on this display\n");
      return 77;
    }

  {
    g_autofree char *theme_name = gtk_icon_theme_get_theme_name (theme);

    g_print ("checking against the \"%s\" theme\n", theme_name);
  }

  for (guint i = 0; i < sorted->len; i++)
    {
      const char *name = g_ptr_array_index (sorted, i);

      if (!gtk_icon_theme_has_icon (theme, name))
        {
          g_printerr ("MISSING %s\n", name);
          missing++;
        }
    }

  g_assert_cmpuint (missing, ==, 0);
  g_print ("ok - every icon name exists in the theme\n");

  return EXIT_SUCCESS;
}
