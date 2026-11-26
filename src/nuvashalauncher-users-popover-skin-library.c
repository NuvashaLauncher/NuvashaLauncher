/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Local/default skin library management. */
#include "nuvashalauncher-users-popover-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-i18n.h"
#include <string.h>

char *
skin_directory_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "nuvashalauncher", "skins", NULL);
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
    g_autofree char *identity = skin_name_for_url (url);
    g_autofree char *name = g_strconcat (identity, ".png", NULL);
    g_autofree char *dir = skin_directory_path ();
    return g_build_filename (dir, name, NULL);
  }
}

char *
cached_cape_path (const char *url)
{
  g_autofree char *identity = NULL;
  g_autofree char *name = NULL;
  g_autofree char *dir = NULL;

  if (url == NULL || (!g_str_has_prefix (url, "http://") &&
                      !g_str_has_prefix (url, "https://")))
    return NULL;
  identity = skin_name_for_url (url);
  name = g_strconcat (identity, ".png", NULL);
  dir = skin_directory_path ();
  return g_build_filename (dir, "capes", name, NULL);
}

typedef struct
{
  const char *name;
  const char *url;
} DefaultSkinEntry;

/* Official Mojang default skins fetched on demand from textures.minecraft.net
 * and cached locally rather than bundled in the repository. */
static const DefaultSkinEntry default_skins[] = {
  { "Steve",  "https://textures.minecraft.net/texture/31f477eb1a7beee631c2ca64d06f8f68fa93a3386d04452ab27f43acdf1b60cb" },
  { "Alex",   "https://textures.minecraft.net/texture/1abc803022d8300ab7578b189294cce39622d9a404cdc00d3feacfdf45be6981" },
  { "Sunny",  "https://textures.minecraft.net/texture/a3bd16079f764cd541e072e888fe43885e711f98658323db0f9a6045da91ee7a" },
  { "Kai",    "https://textures.minecraft.net/texture/e5cdc3243b2153ab28a159861be643a4fc1e3c17d291cdd3e57a7f370ad676f3" },
  { "Makena", "https://textures.minecraft.net/texture/dc0fcfaf2aa040a83dc0de4e56058d1bbb2ea40157501f3e7d15dc245e493095" },
  { "Zuri",   "https://textures.minecraft.net/texture/f5dddb41dcafef616e959c2817808e0be741c89ffbfed39134a13e75b811863d" },
  { "Efe",    "https://textures.minecraft.net/texture/daf3d88ccb38f11f74814e92053d92f7728ddb1a7955652a60e30cb27ae6659f" },
  { "Ari",    "https://textures.minecraft.net/texture/4c05ab9e07b3505dc3ec11370c3bdce5570ad2fb2b562e9b9dd9cf271f81aa44" },
  { "Noor",   "https://textures.minecraft.net/texture/90e75cd429ba6331cd210b9bd19399527ee3bab467b5a9f61cb8a27b177f6789" },
};

const char *
nuvashalauncher_users_default_steve_url (void)
{
  return default_skins[0].url;
}

gboolean
is_default_skin_alias (const char *alias)
{
  if (alias == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (default_skins); i++)
    if (g_ascii_strcasecmp (alias, default_skins[i].name) == 0)
      return TRUE;
  return FALSE;
}

gboolean
skin_url_is_default (const char *url)
{
  g_autofree char *defaults = NULL;
  g_autofree char *path = NULL;
  g_autofree char *prefix = NULL;
  g_autoptr (GFile) file = NULL;

  if (url == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (default_skins); i++)
    if (g_strcmp0 (url, default_skins[i].url) == 0)
      return TRUE;
  if (!g_str_has_prefix (url, "file://"))
    return FALSE;
  defaults =
    g_build_filename (g_get_user_data_dir (), "nuvashalauncher", "skins", "defaults", NULL);
  file = g_file_new_for_uri (url);
  path = g_file_get_path (file);
  prefix = g_strconcat (defaults, G_DIR_SEPARATOR_S, NULL);
  return path != NULL && g_str_has_prefix (path, prefix);
}

void
append_local_skins (GtkListBox                   *skins,
                    GHashTable                   *skin_urls,
                    GHashTable                   *deleted_skin_ids,
                    NuvashaLauncherSkinRowFactory row_factory,
                    gpointer                      user_data)
{
  g_autofree char *directory = skin_directory_path ();
  g_autoptr (GDir) dir = NULL;
  const char *name;

  if (!nuvashalauncher_fs_make_directory (directory, 0755, NULL) ||
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
      if (uri == NULL || g_hash_table_contains (skin_urls, identity) ||
          g_hash_table_contains (deleted_skin_ids, identity))
        continue;
      g_autofree char *display_name = skin_display_name (name);
      g_autoptr (AdwActionRow) row = row_factory (display_name, uri, user_data);
      gtk_list_box_append (skins, GTK_WIDGET (row));
    }
}

AdwActionRow *
append_default_skins (GtkListBox                   *skins,
                      GHashTable                   *skin_urls,
                      GHashTable                   *deleted_skin_ids,
                      gboolean                     *defaults_added,
                      NuvashaLauncherSkinRowFactory row_factory,
                      gpointer                      user_data)
{
  AdwActionRow *steve_row = NULL;

  if (*defaults_added)
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (default_skins); i++)
    {
      const char *name = default_skins[i].name;
      const char *url = default_skins[i].url;
      g_autofree char *identity = skin_name_for_url (url);

      if (!g_hash_table_contains (skin_urls, identity) &&
          !g_hash_table_contains (deleted_skin_ids, identity))
        {
          g_autoptr (AdwActionRow) row = row_factory (name, url, user_data);
          gtk_list_box_append (skins, GTK_WIDGET (row));
          if (g_str_equal (name, "Steve"))
            steve_row = row;
        }
    }
  *defaults_added = TRUE;
  return steve_row;
}
