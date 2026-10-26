/* Local/default skin library management. */
#include "nuvasha-users-popover-private.h"
#include "nuvasha-fs.h"
#include "nuvasha-i18n.h"
#include <string.h>

char *
skin_directory_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "nuvasha", "skins", NULL);
}

char *
skin_name_for_url (const char *url)
{
  if (url != NULL && (g_str_has_prefix (url, "http://") ||
                      g_str_has_prefix (url, "https://")))
    return g_compute_checksum_for_string (G_CHECKSUM_SHA256, url, -1);
  if (url != NULL && g_str_has_prefix (url, "file://"))
    {
      g_autoptr (GFile) file = g_file_new_for_uri (url);
      g_autofree char *basename = g_file_get_basename (file);
      if (basename != NULL)
        {
          gsize length = strlen (basename);
          if (length > 4 && g_ascii_strcasecmp (basename + length - 4, ".png") == 0)
            basename[length - 4] = '\0';
          return g_strdup (basename);
        }
    }
  return g_strdup (_("Skin"));
}

char *
skin_display_name (const char *filename)
{
  char *name = g_strdup (filename);
  gsize length;

  if (name == NULL)
    return NULL;
  length = strlen (name);
  if (length > 4 && g_ascii_strcasecmp (name + length - 4, ".png") == 0)
    name[length - 4] = '\0';
  return name;
}

char *
unique_skin_filename (const char *directory, const char *filename)
{
  g_autofree char *path = g_build_filename (directory, filename, NULL);
  const char *extension = NULL;
  g_autofree char *stem = NULL;

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return g_strdup (filename);

  {
    gsize length = strlen (filename);
    if (length > 4 &&
        g_ascii_strcasecmp (filename + length - 4, ".png") == 0)
      extension = filename + length - 4;
  }
  if (extension == NULL)
    extension = filename + strlen (filename);
  stem = g_strndup (filename, extension - filename);

  for (guint suffix = 1; ; suffix++)
    {
      g_autofree char *candidate =
        g_strdup_printf ("%s %u%s", stem, suffix, extension);
      g_autofree char *candidate_path =
        g_build_filename (directory, candidate, NULL);
      if (!g_file_test (candidate_path, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&candidate);
    }
}

char *
cached_skin_path (const char *url)
{
  if (url == NULL || (!g_str_has_prefix (url, "http://") &&
                      !g_str_has_prefix (url, "https://")))
    return NULL;
  {
    g_autofree char *name = g_strconcat (skin_name_for_url (url), ".png", NULL);
    return g_build_filename (skin_directory_path (), name, NULL);
  }
}

char *
cached_cape_path (const char *url)
{
  g_autofree char *name = NULL;

  if (url == NULL || (!g_str_has_prefix (url, "http://") &&
                      !g_str_has_prefix (url, "https://")))
    return NULL;
  name = g_strconcat (skin_name_for_url (url), ".png", NULL);
  return g_build_filename (skin_directory_path (), "capes", name, NULL);
}

gboolean
is_default_skin_alias (const char *alias)
{
  static const char *defaults[] = {
    "STEVE", "ALEX", "SUNNY", "KAI", "MAKENA", "ZURI", "EFE", "ARI", "NOOR"
  };
  if (alias == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (defaults); i++)
    if (g_ascii_strcasecmp (alias, defaults[i]) == 0)
      return TRUE;
  return FALSE;
}

gboolean
skin_url_is_default (const char *url)
{
  g_autofree char *defaults =
    g_build_filename (g_get_user_data_dir (), "nuvasha", "skins", "defaults", NULL);
  g_autofree char *path = NULL;
  g_autofree char *prefix = NULL;
  g_autoptr (GFile) file = NULL;

  if (url == NULL || !g_str_has_prefix (url, "file://"))
    return FALSE;
  file = g_file_new_for_uri (url);
  path = g_file_get_path (file);
  prefix = g_strconcat (defaults, G_DIR_SEPARATOR_S, NULL);
  return path != NULL && g_str_has_prefix (path, prefix);
}

void
append_local_skins (SkinMenu *menu)
{
  g_autofree char *directory = skin_directory_path ();
  g_autoptr (GDir) dir = NULL;
  const char *name;

  if (!nuvasha_fs_make_directory (directory, 0755, NULL) ||
      (dir = g_dir_open (directory, 0, NULL)) == NULL)
    return;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *path = NULL;
      g_autofree char *uri = NULL;
      if (strlen (name) <= 4 ||
          g_ascii_strcasecmp (name + strlen (name) - 4, ".png") != 0)
        continue;
      /* SHA-256 URL caches are refreshed from the profile response. They are
       * not user-imported skins; keeping them out of this local-library scan
       * also prevents old cape caches from appearing as skins. */
      if (strlen (name) == 68)
        {
          gboolean hash_name = TRUE;
          for (gsize i = 0; i < 64; i++)
            if (!g_ascii_isxdigit (name[i]))
              { hash_name = FALSE; break; }
          if (hash_name)
            continue;
        }
      path = g_build_filename (directory, name, NULL);
      uri = g_filename_to_uri (path, NULL, NULL);
      g_autofree char *identity = uri != NULL ? skin_name_for_url (uri) : NULL;
      if (uri == NULL || g_hash_table_contains (menu->skin_urls, identity) ||
          g_hash_table_contains (menu->deleted_skin_ids, identity))
        continue;
      g_autofree char *display_name = skin_display_name (name);
      gtk_list_box_append (menu->skins,
                           GTK_WIDGET (skin_row_new (menu, display_name, uri)));
    }
}

AdwActionRow *
append_default_skins (SkinMenu *menu)
{
  static const char *defaults[] = {
    "Steve", "Alex", "Sunny", "Kai", "Makena", "Zuri", "Efe", "Ari", "Noor"
  };
  g_autofree char *source_dir = g_build_filename (nuvasha_DATA_DIR, "skins", "defaults", NULL);
  g_autofree char *source_fallback = g_build_filename (nuvasha_SOURCE_DATA_DIR, "skins", "defaults", NULL);
  g_autofree char *target_dir = g_build_filename (g_get_user_data_dir (), "nuvasha", "skins", "defaults", NULL);
  AdwActionRow *steve_row = NULL;

  if (menu->defaults_added || !nuvasha_fs_make_directory (target_dir, 0755, NULL))
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (defaults); i++)
    {
      const char *name = defaults[i];
      g_autofree char *filename = g_strconcat (name, ".png", NULL);
      g_autofree char *source_path = g_build_filename (source_dir, filename, NULL);
      g_autofree char *fallback_path = g_build_filename (source_fallback, filename, NULL);
      g_autofree char *target_path = g_build_filename (target_dir, filename, NULL);
      g_autofree char *uri = NULL;
      g_autoptr (GFile) source = NULL;
      g_autoptr (GFile) target = NULL;

      if (!g_file_test (source_path, G_FILE_TEST_IS_REGULAR))
        {
          g_clear_pointer (&source_path, g_free);
          source_path = g_strdup (fallback_path);
        }
      if (!g_file_test (source_path, G_FILE_TEST_IS_REGULAR))
        continue;
      source = g_file_new_for_path (source_path);
      target = g_file_new_for_path (target_path);
      if (!g_file_test (target_path, G_FILE_TEST_IS_REGULAR))
        nuvasha_fs_copy_file (source, target, G_FILE_COPY_NONE, NULL, NULL);
      uri = g_filename_to_uri (target_path, NULL, NULL);
      g_autofree char *identity = uri != NULL ? skin_name_for_url (uri) : NULL;
      if (uri != NULL && !g_hash_table_contains (menu->skin_urls, identity) &&
          !g_hash_table_contains (menu->deleted_skin_ids, identity))
        {
          AdwActionRow *row = skin_row_new (menu, name, uri);
          gtk_list_box_append (menu->skins, GTK_WIDGET (row));
          if (g_str_equal (name, "Steve"))
            steve_row = row;
        }
    }
  menu->defaults_added = TRUE;
  return steve_row;
}
