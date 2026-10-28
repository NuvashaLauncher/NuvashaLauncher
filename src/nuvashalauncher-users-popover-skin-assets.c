/* Avatar and skin texture loading. */
#include "nuvashalauncher-users-popover-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-i18n.h"
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
  nuvashalauncherUsersPopover *owner;
  GtkImage *image;
  GHashTable *cache;
  char *url;
} AvatarLoad;

static void
avatar_load_free (AvatarLoad *load)
{
  g_clear_object (&load->image);
  g_clear_pointer (&load->cache, g_hash_table_unref);
  g_free (load->url);
  g_free (load);
}

static void
on_skin_downloaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  /* The image is held by a reference for as long as the fetch lasts, so
     that a row which is gone by the time the skin arrives is not written
     to. */
  AvatarLoad *load = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GdkTexture) skin = NULL;
  g_autoptr (GdkTexture) face = NULL;

  bytes = g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  /* Keep the default avatar when the skin cannot be loaded. */
  if (bytes == NULL)
    goto done;

  skin = gdk_texture_new_from_bytes (bytes, &error);

  if (skin == NULL)
    goto done;

  face = face_texture (skin);

  if (face != NULL)
    {
      g_hash_table_replace (load->cache, g_strdup (load->url),
                            g_object_ref (face));
      gtk_image_set_from_paintable (load->image, GDK_PAINTABLE (face));
    }

done:
  avatar_load_free (load);
}

/* Load the face from @url without blocking the UI. */
void
nuvashalauncher_users_set_avatar (nuvashalauncherUsersPopover *self,
            GtkImage        *image,
            const char       *url)
{
  g_autoptr (GFile) file = NULL;
  GdkTexture *cached = NULL;
  AvatarLoad *load = NULL;

  if (url == NULL || *url == '\0')
    return;

  cached = g_hash_table_lookup (self->avatar_cache, url);
  if (cached != NULL)
    {
      gtk_image_set_from_paintable (image, GDK_PAINTABLE (cached));
      return;
    }

  file = g_file_new_for_uri (url);
  load = g_new0 (AvatarLoad, 1);
  load->image = g_object_ref (image);
  load->cache = g_hash_table_ref (self->avatar_cache);
  load->url = g_strdup (url);
  g_file_load_bytes_async (file, self->avatars, on_skin_downloaded, load);
}

/* The icon every account has, and what an offline account has as its
   avatar: there is nothing behind it to have a picture of. */
void
nuvashalauncher_users_set_avatar_icon (GtkImage *image)
{
  gtk_image_set_from_icon_name (image, "avatar-default-symbolic");
}

gboolean
nuvashalauncher_users_set_local_avatar (GtkImage *image, const char *path)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) skin = gdk_texture_new_from_file (file, &error);
  g_autoptr (GdkTexture) face = NULL;

  if (skin == NULL || (face = face_texture (skin)) == NULL)
    return FALSE;
  gtk_image_set_from_paintable (image, GDK_PAINTABLE (face));
  return TRUE;
}

char *
nuvashalauncher_users_default_steve_path (void)
{
  g_autofree char *global = g_build_filename (g_get_user_data_dir (), "nuvashalauncher", "skins", "defaults", "Steve.png", NULL);
  g_autofree char *installed = g_build_filename (nuvashalauncher_DATA_DIR, "skins", "defaults", "Steve.png", NULL);
  g_autofree char *source = g_build_filename (nuvashalauncher_SOURCE_DATA_DIR, "skins", "defaults", "Steve.png", NULL);
  if (g_file_test (global, G_FILE_TEST_IS_REGULAR)) return g_steal_pointer (&global);
  if (g_file_test (installed, G_FILE_TEST_IS_REGULAR)) return g_steal_pointer (&installed);
  return g_steal_pointer (&source);
}
