/* Skin texture loading and cache management. */

#include "nuvashalauncher-users-popover-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"
#include <libsoup/soup.h>

static SoupSession *skin_session;

typedef struct
{
  nuvashalauncherSkinView *view;
  gboolean cape;
  char *url;
  SoupMessage *message;
} SkinTextureLoad;

static void
skin_texture_load_free (SkinTextureLoad *load)
{
  g_clear_object (&load->view);
  g_clear_object (&load->message);
  g_free (load->url);
  g_free (load);
}

static void
cache_skin_bytes (nuvashalauncherSkinView *view, const char *url, GBytes *bytes,
                  gboolean cape)
{
  g_autofree char *cache_directory = skin_directory_path ();
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
                                                          url, -1);
  g_autofree char *path = cape ? cached_cape_path (url) : cached_skin_path (url);
  gsize length = 0;
  gconstpointer data = g_bytes_get_data (bytes, &length);

  {
    GHashTable *deleted = g_object_get_data (G_OBJECT (view),
                                             "nuvashalauncher-deleted-skin-ids");
    if (!cape && deleted != NULL && g_hash_table_contains (deleted, hash))
      return;
  }
  if (cape)
    {
      g_autofree char *cape_directory =
        g_build_filename (cache_directory, "capes", NULL);
      g_clear_pointer (&cache_directory, g_free);
      cache_directory = g_steal_pointer (&cape_directory);
    }
  if (nuvashalauncher_fs_make_directory (cache_directory, 0755, NULL))
    nuvashalauncher_fs_replace_contents (path, data, (gssize) length, 0600, NULL);
}

static void
finish_view_texture_load (SkinTextureLoad *load, GBytes *bytes, GError *error)
{
  g_autoptr (GdkTexture) texture = NULL;

  if (bytes != NULL)
    texture = gdk_texture_new_from_bytes (bytes, &error);
  if (bytes != NULL && load->url != NULL &&
      (g_str_has_prefix (load->url, "http://") ||
       g_str_has_prefix (load->url, "https://")))
     cache_skin_bytes (load->view, load->url, bytes, load->cape);
  if (texture == NULL && error != NULL)
    g_warning ("Could not load skin preview texture: %s", error->message);
  if (texture != NULL)
    {
      if (load->cape)
        nuvashalauncher_skin_view_set_cape (load->view, texture);
      else
        nuvashalauncher_skin_view_set_skin (load->view, texture);
    }
  skin_texture_load_free (load);
}

static void
on_view_file_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinTextureLoad *load = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  finish_view_texture_load (load, bytes, error);
}

static void
on_view_http_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinTextureLoad *load = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (
    SOUP_SESSION (source), result, &error);

  if (load->message != NULL &&
      SOUP_STATUS_IS_SUCCESSFUL (soup_message_get_status (load->message)) == FALSE)
    {
      g_clear_pointer (&bytes, g_bytes_unref);
      g_clear_error (&error);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "skin request returned HTTP status %u",
                   soup_message_get_status (load->message));
    }

  finish_view_texture_load (load, bytes, error);
}

void
load_view_texture (nuvashalauncherSkinView *view, const char *url, gboolean cape)
{
  g_autoptr (GFile) file = NULL;
  g_autofree char *cached = NULL;
  SkinTextureLoad *load;

  if (url == NULL || *url == '\0')
    {
      if (cape)
        nuvashalauncher_skin_view_set_cape (view, NULL);
      return;
    }
  if (g_str_has_prefix (url, "http://") || g_str_has_prefix (url, "https://"))
    {
      cached = cape ? cached_cape_path (url) : cached_skin_path (url);
      if (cached != NULL && g_file_test (cached, G_FILE_TEST_IS_REGULAR))
        file = g_file_new_for_path (cached);
    }
  if (file == NULL)
    file = g_file_new_for_uri (url);
  load = g_new0 (SkinTextureLoad, 1);
  load->view = g_object_ref (view);
  load->cape = cape;
  load->url = g_strdup (url);

  if ((g_str_has_prefix (url, "http://") ||
       g_str_has_prefix (url, "https://")) &&
      (cached == NULL || !g_file_test (cached, G_FILE_TEST_IS_REGULAR)))
    {
      if (skin_session == NULL)
        skin_session = soup_session_new ();
      load->message = soup_message_new ("GET", url);
      soup_session_send_and_read_async (skin_session, load->message,
                                        G_PRIORITY_DEFAULT, NULL,
                                        on_view_http_loaded, load);
    }
  else
    g_file_load_bytes_async (file, NULL, on_view_file_loaded, load);
}
