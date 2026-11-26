/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Avatar and skin texture loading. */
#include "nuvashalauncher-users-popover-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-minecraft-services.h"
#include <libsoup/soup.h>

#define AVATAR_SIZE 40
#define SKIN_SIZE 64
#define FACE_SIZE 8
#define FACE_X 8
#define FACE_Y 8

GdkTexture *
face_texture (GdkTexture *skin)
{
  gsize skin_stride = 0;
  gsize face_stride = (gsize) AVATAR_SIZE * 4;
  g_autofree guchar *pixels = NULL;
  g_autofree guchar *face = NULL;
  g_autoptr (GBytes) bytes = NULL;

  if (skin == NULL || gdk_texture_get_width (skin) != SKIN_SIZE ||
      (gdk_texture_get_height (skin) != SKIN_SIZE &&
       gdk_texture_get_height (skin) != SKIN_SIZE / 2))
    return NULL;

  skin_stride = (gsize) SKIN_SIZE * 4;
  pixels = g_malloc (skin_stride * SKIN_SIZE);

  /* What comes back is four bytes to a pixel, blue first, which is a
     format a texture of its own is made of. */
  gdk_texture_download (skin, pixels, skin_stride);

  face = g_malloc0 (face_stride * AVATAR_SIZE);

  for (int y = 0; y < AVATAR_SIZE; y++)
    {
      for (int x = 0; x < AVATAR_SIZE; x++)
        {
          int from_x = FACE_X + x * FACE_SIZE / AVATAR_SIZE;
          int from_y = FACE_Y + y * FACE_SIZE / AVATAR_SIZE;
          const guchar *from = pixels + (gsize) from_y * skin_stride +
                               (gsize) from_x * 4;

          memcpy (face + (gsize) y * face_stride + (gsize) x * 4, from, 4);
        }
    }

  bytes = g_bytes_new_take (g_steal_pointer (&face),
                            face_stride * AVATAR_SIZE);
  return gdk_memory_texture_new (AVATAR_SIZE, AVATAR_SIZE,
                                 GDK_MEMORY_B8G8R8A8, bytes, face_stride);
}

typedef struct
{
  NuvashaLauncherUsersPopover *owner;
  GtkImage *image;
  GHashTable *cache;
  SoupMessage *message;
  char *url;
} AvatarLoad;

static void
avatar_load_free (AvatarLoad *load)
{
  g_clear_object (&load->image);
  g_clear_pointer (&load->cache, g_hash_table_unref);
  g_clear_object (&load->message);
  g_free (load->url);
  g_free (load);
}

static void
apply_avatar_bytes (AvatarLoad *load, GBytes *bytes)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) skin = NULL;
  g_autoptr (GdkTexture) face = NULL;

  if (bytes == NULL)
    return;

  skin = gdk_texture_new_from_bytes (bytes, &error);
  if (skin == NULL)
    return;

  face = face_texture (skin);
  if (face == NULL)
    return;

  if (g_str_has_prefix (load->url, "http://") ||
      g_str_has_prefix (load->url, "https://"))
    {
      g_autofree char *cached = cached_skin_path (load->url);
      if (cached != NULL && !g_file_test (cached, G_FILE_TEST_IS_REGULAR))
        {
          g_autofree char *dir = g_path_get_dirname (cached);
          gsize len = 0;
          const char *raw = g_bytes_get_data (bytes, &len);
          if (nuvashalauncher_fs_make_directory (dir, 0755, NULL))
            nuvashalauncher_fs_replace_contents (cached, raw, (gssize) len, 0600, NULL);
        }
    }
  g_hash_table_replace (load->cache, g_strdup (load->url),
                        g_object_ref (face));
  gtk_image_set_from_paintable (load->image, GDK_PAINTABLE (face));
}

static void
on_skin_file_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AvatarLoad *load = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  apply_avatar_bytes (load, bytes);
  avatar_load_free (load);
}

static void
on_skin_http_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AvatarLoad *load = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (
    SOUP_SESSION (source), result, &error);
  guint status = load->message != NULL ? soup_message_get_status (load->message) : 0;

  if (bytes != NULL && (load->message == NULL || SOUP_STATUS_IS_SUCCESSFUL (status)))
    apply_avatar_bytes (load, bytes);
  avatar_load_free (load);
}

gboolean
nuvashalauncher_users_network_disabled (void)
{
  return g_getenv ("NUVASHALAUNCHER_TEST_NO_NETWORK") != NULL;
}

/* Load the face from @url without blocking the UI. */
void
nuvashalauncher_users_set_avatar (NuvashaLauncherUsersPopover *self,
            GtkImage        *image,
            const char       *url)
{
  GdkTexture *cached = NULL;
  AvatarLoad *load = NULL;
  gboolean is_http;

  if (url == NULL || *url == '\0')
    return;

  cached = g_hash_table_lookup (self->avatar_cache, url);
  if (cached != NULL)
    {
      gtk_image_set_from_paintable (image, GDK_PAINTABLE (cached));
      return;
    }

  is_http = g_str_has_prefix (url, "http://") || g_str_has_prefix (url, "https://");
  if (is_http)
    {
      g_autofree char *disk_cached = cached_skin_path (url);
      if (disk_cached != NULL && nuvashalauncher_users_set_local_avatar (image, disk_cached))
        return;
      if (nuvashalauncher_users_network_disabled ())
        return;
    }

  load = g_new0 (AvatarLoad, 1);
  load->image = g_object_ref (image);
  load->cache = g_hash_table_ref (self->avatar_cache);
  load->url = g_strdup (url);

  if (is_http)
    {
      g_autoptr (SoupSession) session = nuvashalauncher_minecraft_services_session ();
      load->message = soup_message_new ("GET", url);
      soup_session_send_and_read_async (session, load->message,
                                        G_PRIORITY_DEFAULT, self->avatars,
                                        on_skin_http_loaded, load);
    }
  else
    {
      g_autoptr (GFile) file = g_file_new_for_uri (url);
      g_file_load_bytes_async (file, self->avatars, on_skin_file_loaded, load);
    }
}

/* The icon every account has, and what an offline account has as its
   avatar: there is nothing behind it to have a picture of. */
void
nuvashalauncher_users_set_avatar_icon (GtkImage *image)
{
  gtk_image_set_from_icon_name (image, "avatar-default-symbolic");
}

static GHashTable *
local_avatar_cache (void)
{
  GApplication *app = g_application_get_default ();
  GHashTable *cache;

  if (app == NULL)
    return NULL;

  cache = g_object_get_data (G_OBJECT (app), "nuvashalauncher-local-avatar-cache");
  if (cache == NULL)
    {
      cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
      g_object_set_data_full (G_OBJECT (app), "nuvashalauncher-local-avatar-cache",
                              cache, (GDestroyNotify) g_hash_table_unref);
    }

  return cache;
}

gboolean
nuvashalauncher_users_set_local_avatar (GtkImage *image, const char *path)
{
  if (path == NULL || *path == '\0')
    return FALSE;

  GHashTable *cache = local_avatar_cache ();
  GdkTexture *cached = cache != NULL ? g_hash_table_lookup (cache, path) : NULL;

  if (cached != NULL)
    {
      gtk_image_set_from_paintable (image, GDK_PAINTABLE (cached));
      return TRUE;
    }

  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) skin = gdk_texture_new_from_file (file, &error);
  g_autoptr (GdkTexture) face = NULL;

  if (skin == NULL || (face = face_texture (skin)) == NULL)
    return FALSE;
  if (cache != NULL)
    g_hash_table_replace (cache, g_strdup (path), g_object_ref (face));
  gtk_image_set_from_paintable (image, GDK_PAINTABLE (face));
  return TRUE;
}

void
nuvashalauncher_users_show_account_head (NuvashaLauncherUsersPopover  *owner,
                                         GtkImage                     *image,
                                         const NuvashaLauncherAccount *account)
{
  const char *url = account != NULL ? account->avatar_url : NULL;

  nuvashalauncher_users_set_avatar_icon (image);
  if (url == NULL || *url == '\0')
    return;

  if (g_str_has_prefix (url, "file://"))
    {
      g_autoptr (GFile) file = g_file_new_for_uri (url);
      g_autofree char *path = g_file_get_path (file);

      nuvashalauncher_users_set_local_avatar (image, path);
      return;
    }

  /* The skin of a profile is kept on disk the first time it is fetched, so
   * every later head is read from there at once instead of waiting on the
   * network. */
  {
    g_autofree char *cached = cached_skin_path (url);

    if (cached != NULL && nuvashalauncher_users_set_local_avatar (image, cached))
      return;
  }
  if (owner != NULL)
    nuvashalauncher_users_set_avatar (owner, image, url);
}
