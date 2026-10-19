/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-users-popover.h"
#include "nuvasha-i18n.h"

#include "nuvasha-offline-account-dialog.h"
#include "nuvasha-sign-in-dialog.h"
#include "nuvasha-skin-view.h"
#include "nuvasha-fs.h"
#include "nuvasha-ui-feedback.h"

#include <libsoup/soup.h>

/* How wide the list is: enough for a name and its kind to be read, and no
 * wider than the header the button of it sits in. */
#define USERS_WIDTH 360
#define USERS_ROWS_WIDTH 336

/* A skin is a picture of 64 by 64 and the face is 8 by 8 of it, in the
 * middle row. What is shown here is that face, at the size a row is
 * tall. */
#define AVATAR_SIZE 40
/* The header button shows the avatar alone, at the size of the other
 * header icons rather than a row's. */
#define HEADER_AVATAR_SIZE 16

struct _NuvashaUsersPopover
{
  GtkPopover parent_instance;

  NuvashaAccounts *accounts;

  GtkListBox *list_box;      /* the accounts, one row each */
  GtkScrolledWindow *list_scroller; /* what holds the list */
  GtkLabel *error_label;     /* what went wrong, when something did */
  GtkImage *avatar_image;
  GtkBox *button_content;    /* what the header button shows */

  /* Avatars are pictures of the internet: they are fetched after the row
     that wants one is on screen, and a fetch that is still running when
     the rows are built again is stopped with this. */
  GCancellable *avatars;
  GHashTable *avatar_cache;

  char *active_name;
};

G_DEFINE_FINAL_TYPE (NuvashaUsersPopover, nuvasha_users_popover, GTK_TYPE_POPOVER)

/* A skin is a picture of 64 by 64 and the face of it is 8 by 8 of that,
 * in the middle of the top of the picture. */
#define SKIN_SIZE 64
#define FACE_SIZE 8
#define FACE_X 8
#define FACE_Y 8

/* The face out of a skin, as a texture the image widget can show.
 *
 * The picture is read into memory, the face of it is copied out of the
 * middle of the top, and every pixel of that face is grown to the size
 * it is shown at: a skin is drawn in whole pixels and there is nothing
 * between them that would be worth smoothing. */
static GdkTexture *
face_texture (GdkTexture *skin)
{
  gsize skin_stride = 0;
  gsize face_stride = (gsize) AVATAR_SIZE * 4;
  g_autofree guchar *pixels = NULL;
  g_autofree guchar *face = NULL;

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

  return gdk_memory_texture_new (AVATAR_SIZE, AVATAR_SIZE,
                                 GDK_MEMORY_B8G8R8A8,
                                 g_bytes_new_take (g_steal_pointer (&face),
                                                  face_stride * AVATAR_SIZE),
                                 face_stride);
}

typedef struct
{
  NuvashaUsersPopover *owner;
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
static void
set_avatar (NuvashaUsersPopover *self,
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
static void
set_avatar_icon (GtkImage *image)
{
  gtk_image_set_from_icon_name (image, "avatar-default-symbolic");
}

static gboolean
set_local_avatar (GtkImage *image, const char *path)
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

static char *
default_steve_path (void)
{
  g_autofree char *global = g_build_filename (g_get_user_data_dir (), "nuvasha", "skins", "defaults", "Steve.png", NULL);
  g_autofree char *installed = g_build_filename (nuvasha_DATA_DIR, "skins", "defaults", "Steve.png", NULL);
  g_autofree char *source = g_build_filename (nuvasha_SOURCE_DATA_DIR, "skins", "defaults", "Steve.png", NULL);
  if (g_file_test (global, G_FILE_TEST_IS_REGULAR)) return g_steal_pointer (&global);
  if (g_file_test (installed, G_FILE_TEST_IS_REGULAR)) return g_steal_pointer (&installed);
  return g_steal_pointer (&source);
}

typedef struct
{
  NuvashaUsersPopover *owner;
  GtkImage *image;
  NuvashaSkinView *view;
  AdwDialog *dialog;
  GtkListBox *skins;
  GtkDropDown *capes;
  GtkStringList *cape_list;
  GPtrArray *cape_ids;
  GPtrArray *cape_urls;
  GHashTable *skin_urls;
  AdwActionRow *current_skin_row;
  GtkButton *remove_button;
  gboolean defaults_added;
  SoupMessage *profile_message;
  SoupMessage *cape_message;
  GHashTable *deleted_skin_ids;
  NuvashaAccount *account;
} SkinMenu;

static AdwActionRow *skin_row_new (SkinMenu *menu, const char *name,
                                   const char *url);

static char *
skin_directory_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "nuvasha", "skins", NULL);
}

static char *
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
          char *suffix = g_strrstr (basename, ".png");
          if (suffix != NULL && suffix[4] == '\0')
            *suffix = '\0';
          return g_strdup (basename);
        }
    }
  return g_strdup (_("Skin"));
}

static char *
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

static gboolean
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

static gboolean
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

static void
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
      if (!g_str_has_suffix (name, ".png"))
        continue;
      path = g_build_filename (directory, name, NULL);
      uri = g_filename_to_uri (path, NULL, NULL);
      g_autofree char *identity = uri != NULL ? skin_name_for_url (uri) : NULL;
      if (uri == NULL || g_hash_table_contains (menu->skin_urls, identity) ||
          g_hash_table_contains (menu->deleted_skin_ids, identity))
        continue;
      gtk_list_box_append (menu->skins,
                           GTK_WIDGET (skin_row_new (menu, name, uri)));
    }
}

static AdwActionRow *
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

typedef struct
{
  NuvashaSkinView *view;
  gboolean cape;
  char *url;
} SkinTextureLoad;

static char *skin_directory_path (void);

static void
skin_texture_load_free (SkinTextureLoad *load)
{
  g_clear_object (&load->view);
  g_free (load->url);
  g_free (load);
}

static void
cache_skin_bytes (NuvashaSkinView *view, const char *url, GBytes *bytes)
{
  g_autofree char *cache_directory = skin_directory_path ();
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
                                                          url, -1);
  g_autofree char *name = g_strconcat (hash, ".png", NULL);
  g_autofree char *path = g_build_filename (cache_directory, name, NULL);
  gsize length = 0;
  gconstpointer data = g_bytes_get_data (bytes, &length);

  {
    GHashTable *deleted = g_object_get_data (G_OBJECT (view),
                                             "nuvasha-deleted-skin-ids");
    if (deleted != NULL && g_hash_table_contains (deleted, hash))
      return;
  }
  if (nuvasha_fs_make_directory (cache_directory, 0755, NULL))
    nuvasha_fs_replace_contents (path, data, (gssize) length, 0600, NULL);
}

static void
on_view_texture_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinTextureLoad *load = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);
  g_autoptr (GdkTexture) texture = NULL;

  if (bytes != NULL)
    texture = gdk_texture_new_from_bytes (bytes, &error);
  if (bytes != NULL && !load->cape && load->url != NULL &&
      (g_str_has_prefix (load->url, "http://") ||
       g_str_has_prefix (load->url, "https://")))
    cache_skin_bytes (load->view, load->url, bytes);
  if (texture == NULL && error != NULL)
    g_warning ("Could not load skin preview texture: %s", error->message);
  if (texture != NULL)
    {
      if (load->cape)
        nuvasha_skin_view_set_cape (load->view, texture);
      else
        nuvasha_skin_view_set_skin (load->view, texture);
    }
  skin_texture_load_free (load);
}

static void
load_view_texture (NuvashaSkinView *view, const char *url, gboolean cape)
{
  g_autoptr (GFile) file = NULL;
  SkinTextureLoad *load;

  if (url == NULL || *url == '\0')
    {
      if (cape)
        nuvasha_skin_view_set_cape (view, NULL);
      return;
    }
  file = g_file_new_for_uri (url);
  load = g_new0 (SkinTextureLoad, 1);
  load->view = g_object_ref (view);
  load->cape = cape;
  load->url = g_strdup (url);
  g_file_load_bytes_async (file, NULL, on_view_texture_loaded, load);
}

static void
on_capes_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinMenu *menu = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  g_autoptr (JsonParser) parser = NULL;
  JsonNode *root = NULL;
  JsonArray *capes = NULL;

  if (bytes == NULL ||
      menu->cape_message == NULL ||
      soup_message_get_status (menu->cape_message) != 200)
    goto done;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, g_bytes_get_data (bytes, NULL),
                                   (gssize) g_bytes_get_size (bytes), &error))
    goto done;

  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root))
    goto done;
  {
    JsonNode *node = json_object_get_member (json_node_get_object (root), "capes");
    if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
      goto done;
    capes = json_node_get_array (node);
  }

  for (guint i = 0; i < json_array_get_length (capes); i++)
    {
      JsonNode *item = json_array_get_element (capes, i);
      JsonObject *object;
      const char *id;
      const char *alias;
      const char *url;
      const char *state;

      if (!JSON_NODE_HOLDS_OBJECT (item))
        continue;
      object = json_node_get_object (item);
      id = json_object_get_string_member_with_default (object, "id", NULL);
      alias = json_object_get_string_member_with_default (object, "alias", id);
      url = json_object_get_string_member_with_default (object, "url", NULL);
      state = json_object_get_string_member_with_default (object, "state", NULL);
      if (id == NULL || *id == '\0')
        continue;
      g_ptr_array_add (menu->cape_ids, g_strdup (id));
      g_ptr_array_add (menu->cape_urls, g_strdup (url != NULL ? url : ""));
      gtk_string_list_append (menu->cape_list,
                              alias != NULL && *alias != '\0' ? alias : id);
      if (state != NULL && g_str_equal (state, "ACTIVE"))
        gtk_drop_down_set_selected (menu->capes, menu->cape_ids->len);
    }

done:
  g_object_set_data (G_OBJECT (menu->dialog), "nuvasha-capes-loaded",
                     GINT_TO_POINTER (1));
}

static void
on_cape_selected (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                  gpointer user_data)
{
  SkinMenu *menu = user_data;
  guint selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (object));

  if (selected == 0 || selected - 1 >= menu->cape_urls->len)
    {
      nuvasha_skin_view_set_cape (menu->view, NULL);
      return;
    }
  load_view_texture (menu->view,
                     g_ptr_array_index (menu->cape_urls, selected - 1), TRUE);
}

static void
on_model_toggled (GtkCheckButton *button, gpointer user_data)
{
  SkinMenu *menu = user_data;
  if (gtk_check_button_get_active (button))
    nuvasha_skin_view_set_slim (menu->view,
                            g_object_get_data (G_OBJECT (button), "nuvasha-slim") != NULL);
}

static void
load_capes (SkinMenu *menu)
{
  SoupSession *session;
  SoupMessage *message;
  const char *token;
  char *authorization;

  if (menu->account == NULL || menu->account->type != nuvasha_ACCOUNT_MICROSOFT ||
      menu->account->access_token == NULL ||
      !nuvasha_account_token_is_valid (menu->account, g_get_real_time () / 1000000))
    return;

  session = soup_session_new_with_options ("user-agent", "nuvasha", NULL);
  message = soup_message_new (SOUP_METHOD_GET,
                              "https://api.minecraftservices.com/minecraft/profile");
  token = menu->account->access_token;
  authorization = g_strconcat ("Bearer ", token, NULL);
  soup_message_headers_replace (soup_message_get_request_headers (message),
                                "Authorization", authorization);
  g_free (authorization);
  g_object_set_data_full (G_OBJECT (session), "nuvasha-cape-message", message,
                          g_object_unref);
  menu->cape_message = g_object_ref (message);
  soup_session_send_and_read_async (session, message, G_PRIORITY_DEFAULT, NULL,
                                    on_capes_loaded, menu);
  g_object_unref (session);
}

static void
on_skin_file_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinMenu *menu = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GdkTexture) skin = NULL;
  g_autoptr (GdkTexture) face = NULL;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);
  if (file == NULL)
    goto done;

  {
    g_autofree char *directory = skin_directory_path ();
    g_autofree char *source_uri = g_file_get_uri (file);
    g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
                                                            source_uri, -1);
    g_autofree char *basename = g_strconcat (hash, ".png", NULL);
    g_autofree char *destination_path = NULL;
    g_autoptr (GFile) destination = NULL;

    if (basename == NULL || !nuvasha_fs_make_directory (directory, 0755, NULL))
      goto done;
    destination_path = g_build_filename (directory, basename, NULL);
    destination = g_file_new_for_path (destination_path);
    if (!nuvasha_fs_copy_file (file, destination, G_FILE_COPY_OVERWRITE,
                           NULL, &error))
      goto done;
    g_clear_object (&file);
    file = g_steal_pointer (&destination);
  }

  skin = gdk_texture_new_from_file (file, &error);
  if (skin == NULL ||
      (gdk_texture_get_width (skin) != 64 && gdk_texture_get_width (skin) != 64) ||
      (gdk_texture_get_height (skin) != 32 && gdk_texture_get_height (skin) != 64))
    {
      g_clear_error (&error);
       error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    _("Skin images must be 64x64 or 64x32 PNG files"));
      goto done;
    }

  /* Imported files are immediately usable as the row's preview. Uploading
   * is intentionally separate from choosing a local skin, just like the
   * skin manager in PrismLauncher. */
  face = face_texture (skin);
  if (face != NULL)
    {
      gtk_image_set_from_paintable (menu->image, GDK_PAINTABLE (face));
      {
        char *uri = g_file_get_uri (file);
        AdwActionRow *row = skin_row_new (menu, _("Imported skin"), uri);
        gtk_list_box_append (menu->skins, GTK_WIDGET (row));
        gtk_list_box_select_row (menu->skins, GTK_LIST_BOX_ROW (row));
        load_view_texture (menu->view, uri, FALSE);
        g_free (uri);
      }
    }

done:
  if (error != NULL &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      AdwAlertDialog *alert = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_("Invalid skin"), error->message));
      adw_alert_dialog_add_response (alert, "ok", _("OK"));
      adw_alert_dialog_set_close_response (alert, "ok");
      adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (menu->dialog));
    }
  /* The dialog owns the skin list and may still be open after the file
   * chooser returns.  Its small controller is kept for the lifetime of the
   * dialog. */
}

static void
on_import_skin_clicked (GtkButton *button, gpointer user_data)
{
  SkinMenu *source = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
  GtkFileFilter *filter = gtk_file_filter_new ();

  gtk_file_filter_set_name (filter, _("PNG skin texture"));
  gtk_file_filter_add_mime_type (filter, "image/png");
  gtk_file_dialog_set_default_filter (dialog, filter);
  gtk_file_dialog_open (dialog, GTK_WINDOW (root), NULL,
                        on_skin_file_chosen, source);
  g_object_unref (dialog);
}

static void
remove_skin_row (SkinMenu *menu, GtkListBoxRow *row)
{
  const char *url = g_object_get_data (G_OBJECT (row), "nuvasha-skin-url");
  g_autofree char *path = NULL;

  if (url != NULL && g_str_has_prefix (url, "file://"))
    {
      g_autoptr (GFile) file = g_file_new_for_uri (url);
      path = g_file_get_path (file);
    }
  else if (url != NULL && (g_str_has_prefix (url, "http://") ||
                           g_str_has_prefix (url, "https://")))
    {
      g_autofree char *name = g_strconcat (skin_name_for_url (url), ".png", NULL);
      path = g_build_filename (skin_directory_path (), name, NULL);
    }
  if (path != NULL)
    nuvasha_fs_unlink (path, NULL);
  if (url != NULL)
    g_hash_table_add (menu->deleted_skin_ids, g_strdup (skin_name_for_url (url)));

  int index = gtk_list_box_row_get_index (row);
  gtk_list_box_remove (menu->skins, GTK_WIDGET (row));
  row = gtk_list_box_get_row_at_index (menu->skins, index);
  if (row == NULL && index > 0)
    row = gtk_list_box_get_row_at_index (menu->skins, index - 1);
  if (row != NULL)
    gtk_list_box_select_row (menu->skins, row);
  else
    set_avatar_icon (menu->image);
}

static void
on_active_skin_delete_done (GObject *source, GAsyncResult *result,
                            gpointer user_data G_GNUC_UNUSED)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  SoupMessage *message = g_object_get_data (G_OBJECT (source),
                                            "nuvasha-delete-skin-message");

  if (bytes == NULL || message == NULL || soup_message_get_status (message) >= 300)
    g_warning ("Could not delete active Minecraft skin: %s",
               error != NULL ? error->message : "server rejected the request");
}

static void
delete_active_skin (SkinMenu *menu)
{
  SoupSession *session;
  SoupMessage *message;
  char *authorization;

  if (menu->account == NULL || menu->account->type != nuvasha_ACCOUNT_MICROSOFT ||
      menu->account->access_token == NULL)
    return;
  session = soup_session_new_with_options ("user-agent", "nuvasha", NULL);
  message = soup_message_new (SOUP_METHOD_DELETE,
                              "https://api.minecraftservices.com/minecraft/profile/skins/active");
  authorization = g_strconcat ("Bearer ", menu->account->access_token, NULL);
  soup_message_headers_replace (soup_message_get_request_headers (message),
                                "Authorization", authorization);
  g_free (authorization);
  g_object_set_data_full (G_OBJECT (session), "nuvasha-delete-skin-message",
                          message, g_object_unref);
  soup_session_send_and_read_async (session, message, G_PRIORITY_DEFAULT, NULL,
                                    on_active_skin_delete_done, NULL);
  g_object_unref (session);
}

static void
on_remove_skin_confirmed (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinMenu *menu = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
  GtkListBoxRow *row = g_object_get_data (G_OBJECT (source), "nuvasha-skin-row");

  if (g_str_equal (response, "remove") && row != NULL)
    {
      const char *url = g_object_get_data (G_OBJECT (row), "nuvasha-skin-url");
      if (url != NULL && menu->account != NULL &&
          g_strcmp0 (url, menu->account->avatar_url) == 0)
        delete_active_skin (menu);
      remove_skin_row (menu, row);
    }
}

static void
on_remove_skin_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  SkinMenu *menu = user_data;
  GtkListBoxRow *row = gtk_list_box_get_selected_row (menu->skins);
  const char *url;

  if (row == NULL ||
      (url = g_object_get_data (G_OBJECT (row), "nuvasha-skin-url")) == NULL ||
      skin_url_is_default (url))
    return;

  {
    AdwAlertDialog *alert = ADW_ALERT_DIALOG (
      adw_alert_dialog_new (_("Delete skin?"),
                            _("This skin will be removed from the skin library.")));
    adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
    adw_alert_dialog_add_response (alert, "remove", _("Delete"));
    adw_alert_dialog_set_response_appearance (alert, "remove",
                                              ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (alert, "cancel");
    adw_alert_dialog_set_close_response (alert, "cancel");
    g_object_set_data (G_OBJECT (alert), "nuvasha-skin-row", row);
    adw_alert_dialog_choose (alert, GTK_WIDGET (menu->dialog), NULL,
                             on_remove_skin_confirmed, menu);
  }
}

static void
on_open_skins_clicked (GtkButton *button G_GNUC_UNUSED,
                       gpointer user_data G_GNUC_UNUSED)
{
  g_autofree char *directory =
    g_build_filename (g_get_user_data_dir (), "nuvasha", "skins", NULL);
  g_autofree char *uri = NULL;
  g_autoptr (GError) error = NULL;

  if (!nuvasha_fs_make_directory (directory, 0755, NULL))
    return;
  uri = g_filename_to_uri (directory, NULL, NULL);
  if (uri == NULL ||
      !g_app_info_launch_default_for_uri (uri, NULL, &error))
    if (error != NULL)
      g_warning ("Could not open skins folder: %s", error->message);
}

static void
on_export_skin_copied (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GError) error = NULL;

  if (!nuvasha_fs_copy_finish (G_FILE (source), result, &error))
    g_warning ("Could not export skin: %s", error->message);

  g_object_unref (user_data);
}

static void
on_export_skin_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) destination =
    gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);
  GFile *source_file = user_data;

  if (destination != NULL)
    nuvasha_fs_copy_async (source_file, destination, G_FILE_COPY_OVERWRITE,
                           G_PRIORITY_DEFAULT, NULL, NULL, NULL,
                           on_export_skin_copied, source_file);
  else
    g_object_unref (source_file);
}

static void
on_export_skin_clicked (GtkButton *button, gpointer user_data)
{
  SkinMenu *menu = user_data;
  GtkListBoxRow *row = gtk_list_box_get_selected_row (menu->skins);
  const char *url;
  GtkFileDialog *dialog;

  if (row == NULL ||
      (url = g_object_get_data (G_OBJECT (row), "nuvasha-skin-url")) == NULL)
    return;
  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_initial_name (dialog, "skin.png");
  gtk_file_dialog_save (dialog,
                        GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (button))),
                        NULL, on_export_skin_chosen, g_file_new_for_uri (url));
  g_object_unref (dialog);
}

static void
on_skin_selected (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
  SkinMenu *menu = user_data;
  const char *url;

  if (row == NULL)
    return;
  url = g_object_get_data (G_OBJECT (row), "nuvasha-skin-url");
  if (menu->remove_button != NULL)
    gtk_widget_set_sensitive (GTK_WIDGET (menu->remove_button),
                              url != NULL && !skin_url_is_default (url));

  if (url == NULL)
    return;

  /* Selecting a skin is the change action.  The profile service accepts the
   * URL of a skin already owned by the account; keep the row's avatar in sync
   * immediately while the service refresh happens on the next profile load. */
  g_object_set_data_full (G_OBJECT (menu->dialog), "nuvasha-selected-skin",
                          g_strdup (url), g_free);
  gtk_list_box_select_row (list, row);
  set_avatar_icon (menu->image);
  load_view_texture (menu->view, url, FALSE);

  /* Keep nuvasha's cached profile in step with the selection so the account row
   * and the next opening of this manager show the selected skin. */
  if (menu->owner != NULL && menu->account != NULL)
    {
      g_autoptr (NuvashaAccount) account = nuvasha_account_copy (menu->account);
      g_autoptr (GError) error = NULL;
      g_free (account->avatar_url);
      account->avatar_url = g_strdup (url);
      nuvasha_accounts_update (menu->owner->accounts, account, &error);
    }
}

static AdwActionRow *
skin_row_new (SkinMenu *menu, const char *name, const char *url)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkImage *image = GTK_IMAGE (gtk_image_new_from_icon_name ("image-x-generic-symbolic"));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 name != NULL && *name != '\0' ? name : _("Skin"));
  gtk_image_set_pixel_size (image, AVATAR_SIZE);
  set_avatar_icon (image);
  if (url != NULL)
    {
      g_autofree char *identity = skin_name_for_url (url);
      g_hash_table_add (menu->skin_urls, g_strdup (identity));
      g_object_set_data_full (G_OBJECT (row), "nuvasha-skin-url", g_strdup (url), g_free);
      if (g_str_has_prefix (url, "file://"))
        {
          g_autoptr (GFile) file = g_file_new_for_uri (url);
          g_autofree char *path = g_file_get_path (file);
          if (path == NULL || !set_local_avatar (image, path))
            set_avatar_icon (image);
        }
      else
        {
          g_autofree char *cached = cached_skin_path (url);
          if (cached == NULL || !set_local_avatar (image, cached))
            set_avatar (menu->owner, image, url);
        }
    }
  adw_action_row_add_prefix (row, GTK_WIDGET (image));
  return row;
}

static void
append_profile_skins (SkinMenu *menu, const char *json)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonArray *skins;

  if (!json_parser_load_from_data (parser, json, -1, NULL))
    return;
  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root))
    return;
  root = json_object_get_member (json_node_get_object (root), "skins");
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    return;
  skins = json_node_get_array (root);
  for (guint i = 0; i < json_array_get_length (skins); i++)
    {
      JsonObject *skin = json_node_get_object (json_array_get_element (skins, i));
      const char *url = json_object_get_string_member_with_default (skin, "url", NULL);
      const char *alias = json_object_get_string_member_with_default (skin, "alias", NULL);
      if (url != NULL)
        {
          g_autofree char *fallback_name = NULL;
          g_autofree char *identity = skin_name_for_url (url);
          if (is_default_skin_alias (alias))
            {
              if (menu->current_skin_row != NULL)
                {
                  gtk_list_box_remove (menu->skins,
                                       GTK_WIDGET (menu->current_skin_row));
                  menu->current_skin_row = NULL;
                }
              continue;
            }
          if (g_hash_table_contains (menu->skin_urls, identity))
            {
              if (menu->current_skin_row != NULL && alias != NULL && *alias != '\0')
                adw_preferences_row_set_title (
                  ADW_PREFERENCES_ROW (menu->current_skin_row), alias);
              continue;
            }
          if (alias == NULL || *alias == '\0')
            fallback_name = skin_name_for_url (url);
          gtk_list_box_append (menu->skins,
                               GTK_WIDGET (skin_row_new (menu,
                                             alias != NULL && *alias != '\0'
                                               ? alias : fallback_name,
                                             url)));
        }
    }
}

static void
on_profile_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinMenu *menu = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (bytes != NULL && menu->profile_message != NULL &&
      soup_message_get_status (menu->profile_message) == 200)
    {
      gsize length = 0;
      const char *data = g_bytes_get_data (bytes, &length);
      g_autofree char *json = g_strndup (data, length);
      append_profile_skins (menu, json);
    }
  append_default_skins (menu);
  /* Keep the controller alive: rows may be selected after the request
   * completes. */
}

static void
load_profile_skins (SkinMenu *menu)
{
  SoupSession *session;
  SoupMessage *message;
  char *authorization;

  if (menu->account == NULL || menu->account->access_token == NULL)
    return;
  session = soup_session_new_with_options ("user-agent", "nuvasha", NULL);
  message = soup_message_new (SOUP_METHOD_GET,
                              "https://api.minecraftservices.com/minecraft/profile");
  authorization = g_strconcat ("Bearer ", menu->account->access_token, NULL);
  soup_message_headers_replace (soup_message_get_request_headers (message),
                                "Authorization", authorization);
  g_free (authorization);
  g_object_set_data_full (G_OBJECT (session), "nuvasha-profile-message", message,
                          g_object_unref);
  menu->profile_message = g_object_ref (message);
  soup_session_send_and_read_async (session, message, G_PRIORITY_DEFAULT, NULL,
                                    on_profile_loaded, menu);
  g_object_unref (session);
}

static AdwDialog *
skin_menu_new (NuvashaUsersPopover *owner, GtkImage *image, NuvashaAccount *account)
{
  AdwDialog *dialog = adw_dialog_new ();
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  GtkBox *root = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18));
  GtkBox *left = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 10));
  GtkBox *right = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 10));
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  GtkListBox *skins = GTK_LIST_BOX (gtk_list_box_new ());
  NuvashaSkinView *preview = nuvasha_skin_view_new ();
  GtkCheckButton *default_model = GTK_CHECK_BUTTON (
    gtk_check_button_new_with_label (_("Default model")));
  GtkCheckButton *slim_model = GTK_CHECK_BUTTON (
    gtk_check_button_new_with_label (_("Slim model")));
  GtkStringList *cape_list = gtk_string_list_new ((const char *[]) { _("No cape"), NULL });
  GtkDropDown *capes = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (cape_list), NULL));
  GtkButton *import = GTK_BUTTON (gtk_button_new_with_label (_("Import skin…")));
  GtkButton *export = GTK_BUTTON (gtk_button_new_with_label (_("Export skin…")));
  GtkButton *open_skins = GTK_BUTTON (gtk_button_new_from_icon_name ("folder-symbolic"));
  GtkButton *remove = GTK_BUTTON (gtk_button_new_from_icon_name ("user-trash-symbolic"));
  GtkBox *actions = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkBox *action_spacer = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  SkinMenu *data = g_new0 (SkinMenu, 1);

  data->image = g_object_ref (image);
  data->view = g_object_ref (preview);
  data->owner = owner;
  data->dialog = g_object_ref (dialog);
  data->skins = g_object_ref (skins);
  data->cape_list = g_object_ref (cape_list);
  data->cape_ids = g_ptr_array_new_with_free_func (g_free);
  data->cape_urls = g_ptr_array_new_with_free_func (g_free);
  data->skin_urls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  data->deleted_skin_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  data->capes = capes;
  data->account = account;
  data->remove_button = remove;
  g_object_set_data (G_OBJECT (preview), "nuvasha-deleted-skin-ids",
                     data->deleted_skin_ids);
  g_object_set_data (G_OBJECT (slim_model), "nuvasha-slim", GINT_TO_POINTER (1));
  g_signal_connect (import, "clicked", G_CALLBACK (on_import_skin_clicked), data);
  g_signal_connect (export, "clicked", G_CALLBACK (on_export_skin_clicked), data);
  g_signal_connect (open_skins, "clicked", G_CALLBACK (on_open_skins_clicked), NULL);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_skin_clicked), data);
  g_signal_connect (skins, "row-selected", G_CALLBACK (on_skin_selected), data);
  g_signal_connect (default_model, "toggled", G_CALLBACK (on_model_toggled), data);
  g_signal_connect (slim_model, "toggled", G_CALLBACK (on_model_toggled), data);

  gtk_widget_set_size_request (GTK_WIDGET (left), 220, -1);
  gtk_widget_set_hexpand (GTK_WIDGET (right), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (right), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (scroller), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (skins), "boxed-list");
  gtk_widget_set_halign (GTK_WIDGET (preview), GTK_ALIGN_FILL);
  gtk_widget_set_vexpand (GTK_WIDGET (preview), TRUE);
  gtk_widget_set_size_request (GTK_WIDGET (preview), 220, 300);
  gtk_check_button_set_group (slim_model, default_model);
  gtk_check_button_set_active (default_model, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (remove), "destructive-action");
  gtk_widget_set_hexpand (GTK_WIDGET (export), FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (import), FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (remove), FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (open_skins), FALSE);
  gtk_widget_set_halign (GTK_WIDGET (remove), GTK_ALIGN_END);
  gtk_widget_set_tooltip_text (GTK_WIDGET (open_skins), _("Open Folder"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (remove), _("Delete"));
  gtk_widget_set_hexpand (GTK_WIDGET (action_spacer), TRUE);
  gtk_box_append (left, GTK_WIDGET (preview));
  gtk_box_append (left, GTK_WIDGET (capes));
  gtk_box_append (left, GTK_WIDGET (default_model));
  gtk_box_append (left, GTK_WIDGET (slim_model));
  gtk_scrolled_window_set_child (scroller, GTK_WIDGET (skins));
  gtk_box_append (right, GTK_WIDGET (scroller));
  gtk_box_append (actions, GTK_WIDGET (import));
  gtk_box_append (actions, GTK_WIDGET (export));
  gtk_box_append (actions, GTK_WIDGET (action_spacer));
  gtk_box_append (actions, GTK_WIDGET (open_skins));
  gtk_box_append (actions, GTK_WIDGET (remove));
  gtk_box_append (right, GTK_WIDGET (actions));
  gtk_widget_set_margin_start (GTK_WIDGET (root), 24);
  gtk_widget_set_margin_end (GTK_WIDGET (root), 24);
  gtk_widget_set_margin_top (GTK_WIDGET (root), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (root), 24);
  gtk_box_append (root, GTK_WIDGET (left));
  gtk_box_append (root, GTK_WIDGET (right));
  adw_dialog_set_title (dialog, _("Skin"));
  adw_dialog_set_content_width (dialog, 860);
  adw_dialog_set_content_height (dialog, 560);
  adw_header_bar_set_show_back_button (header, FALSE);
  adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (header));
  adw_toolbar_view_set_content (toolbar, GTK_WIDGET (root));
  adw_dialog_set_child (dialog, GTK_WIDGET (toolbar));
  append_local_skins (data);
  if (account->type != nuvasha_ACCOUNT_OFFLINE && account->avatar_url != NULL)
    {
      if (!g_hash_table_contains (data->skin_urls,
                                  skin_name_for_url (account->avatar_url)) &&
          !g_hash_table_contains (data->deleted_skin_ids,
                                  skin_name_for_url (account->avatar_url)))
        {
          g_autofree char *skin_name = skin_name_for_url (account->avatar_url);
          gboolean cached = FALSE;
          if (g_str_has_prefix (account->avatar_url, "http://") ||
              g_str_has_prefix (account->avatar_url, "https://"))
            {
              g_autofree char *cached_name = g_strconcat (skin_name, ".png", NULL);
              g_autofree char *cached_path =
                g_build_filename (skin_directory_path (), cached_name, NULL);
              cached = g_file_test (cached_path, G_FILE_TEST_IS_REGULAR);
            }
          if (!cached)
            {
              data->current_skin_row = skin_row_new (data, skin_name, account->avatar_url);
              gtk_list_box_append (skins, GTK_WIDGET (data->current_skin_row));
            }
          load_view_texture (preview, account->avatar_url, FALSE);
        }
    }
  else
    {
      AdwActionRow *steve_row = append_default_skins (data);
      if (steve_row != NULL)
        gtk_list_box_select_row (skins, GTK_LIST_BOX_ROW (steve_row));
    }
  g_signal_connect (capes, "notify::selected", G_CALLBACK (on_cape_selected), data);
  load_capes (data);
  load_profile_skins (data);
  return dialog;
}

/* Update the header button for the active account. */
static void
update_the_button (NuvashaUsersPopover *self)
{
  NuvashaAccount *account = nuvasha_accounts_get_active (self->accounts);

  g_clear_pointer (&self->active_name, g_free);
  set_avatar_icon (GTK_IMAGE (self->avatar_image));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self), NULL);

  if (account == NULL)
    {
      self->active_name = g_strdup ("");
      return;
    }

  self->active_name = g_strdup (account->name);
  set_avatar_icon (GTK_IMAGE (self->avatar_image));

}


/* ------------------------------------------------------ *
 * One account
 * ------------------------------------------------------ */

static void
show_error (NuvashaUsersPopover *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), message);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), TRUE);
}

/* The account a widget in a row is about, which is the id of the account
   the row was built from. */
static const char *
account_id_of (gpointer widget)
{
  return g_object_get_data (G_OBJECT (widget), "nuvasha-account-id");
}

static AdwDialog *skin_menu_new (NuvashaUsersPopover *owner, GtkImage *image,
                                 NuvashaAccount *account);

static void
open_skin_menu_for_account (NuvashaUsersPopover *self, const char *id,
                            GtkImage *image)
{
  NuvashaAccount *account = nuvasha_accounts_find (self->accounts, id);
  AdwDialog *dialog;

  if (account == NULL || image == NULL)
    return;
  gtk_popover_popdown (GTK_POPOVER (self));
  dialog = skin_menu_new (self, image, account);
  gtk_widget_set_focusable (GTK_WIDGET (dialog), TRUE);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
  gtk_widget_grab_focus (GTK_WIDGET (dialog));
  g_object_unref (dialog);
}

static void
on_avatar_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = account_id_of (button);
  GtkImage *image = GTK_IMAGE (gtk_button_get_child (button));

  open_skin_menu_for_account (self, id, image);
}

static void
on_row_activated (AdwActionRow *row, gpointer user_data G_GNUC_UNUSED)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = account_id_of (row);
  g_autoptr (GError) error = NULL;

  if (id != NULL && !nuvasha_accounts_set_active (self->accounts, id, &error))
    show_error (self, error->message);
  else
    gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_remove_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = g_object_get_data (G_OBJECT (source), "nuvasha-account-id");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (id != NULL && g_strcmp0 (response, "remove") == 0)
    {
      g_autoptr (GError) error = NULL;

      if (!nuvasha_accounts_remove (self->accounts, id, &error))
        show_error (self, error->message);
    }

  g_object_unref (self);
}

/* Removing an account throws away its saved sign-in for good, so it is
 * asked for first. */
static void
on_remove_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  const char *id = account_id_of (button);
  const char *name = g_object_get_data (G_OBJECT (button), "nuvasha-account-name");
  g_autofree char *heading = NULL;
  AdwAlertDialog *alert = NULL;

  if (id == NULL)
    return;

  /* Move focus away from the popover before showing the confirmation. */
  gtk_popover_popdown (GTK_POPOVER (self));

  heading = g_strdup_printf (
    "Remove %s?", name != NULL && *name != '\0' ? name : "this account");

  alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (
      heading, "The account is removed from nuvasha and its saved sign-in is "
               "forgotten. The game itself is not touched."));
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "remove", _("Remove"));
  adw_alert_dialog_set_response_appearance (alert, "remove",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  g_object_set_data_full (G_OBJECT (alert), "nuvasha-account-id",
                          g_strdup (id), g_free);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_remove_chosen,
                           g_object_ref (self));
}

/* One account as a row of the list. The whole row opens the skin settings
 * for the account it is about, rather than requiring a click on the face.
 * The account that is active carries a check mark, and the trash button
 * takes an account out. */
static void
append_account_row (NuvashaUsersPopover *self, const NuvashaAccount *account)
{
  gboolean is_active =
    g_str_equal (nuvasha_accounts_get_active_id (self->accounts) != NULL
                   ? nuvasha_accounts_get_active_id (self->accounts)
                   : "",
                 account->id);
  gboolean needs_sign_in =
    account->type == nuvasha_ACCOUNT_MICROSOFT &&
    !nuvasha_account_token_is_valid (account, g_get_real_time () / 1000000);

  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkImage *image = GTK_IMAGE (gtk_image_new ());
  GtkButton *avatar = GTK_BUTTON (gtk_button_new ());
  GtkButton *remove = GTK_BUTTON (gtk_button_new_from_icon_name ("user-trash-symbolic"));
  g_autofree char *tooltip = NULL;

  /* The name and the kind are plain text: an account name may hold an
     "&". Owned by the row, which is itself the clickable target. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), account->name);

  /* The kind of account is on every row: an offline session and a
     Microsoft one are not the same thing to play with, and the user is
     the one who has to know which one a launch will run as. An account
     whose token has run out says so here, which is earlier than finding
     it out from a launch that stops. */
  adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)),
                               needs_sign_in ? "Microsoft · Needs sign-in"
                                             : nuvasha_account_type_name (
                                                   account->type));

  if (needs_sign_in)
    gtk_widget_add_css_class (GTK_WIDGET (row), "warning");

  set_avatar_icon (GTK_IMAGE (image));
  if (account->type == nuvasha_ACCOUNT_OFFLINE)
    {
      g_autofree char *steve = default_steve_path ();
      if (!set_local_avatar (GTK_IMAGE (image), steve))
        set_avatar_icon (GTK_IMAGE (image));
    }
  else
    set_avatar (self, GTK_IMAGE (image), account->avatar_url);
  gtk_image_set_pixel_size (GTK_IMAGE (GTK_WIDGET (image)), AVATAR_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (image), GTK_ALIGN_CENTER);
  gtk_button_set_child (avatar, GTK_WIDGET (image));
  gtk_button_set_has_frame (avatar, FALSE);
  gtk_widget_set_valign (GTK_WIDGET (avatar), GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (avatar), "nuvasha-account-id",
                          g_strdup (account->id), g_free);
  g_signal_connect (avatar, "clicked", G_CALLBACK (on_avatar_clicked), self);
  g_object_set_data (G_OBJECT (row), "nuvasha-account-image", image);
  adw_action_row_add_prefix (ADW_ACTION_ROW (GTK_WIDGET (row)),
                             GTK_WIDGET (avatar));

  /* The mark of the one a launch plays as, at the far end of the row. */
  if (is_active)
    {
      GtkImage *check = GTK_IMAGE (gtk_image_new_from_icon_name ("object-select-symbolic"));

      gtk_widget_add_css_class (GTK_WIDGET (check), "accent");
      gtk_widget_set_valign (GTK_WIDGET (check), GTK_ALIGN_CENTER);
      adw_action_row_add_suffix (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (check));
    }

  tooltip = g_strdup_printf (_("Remove \"%s\""), account->name);
  gtk_widget_set_tooltip_text (GTK_WIDGET (remove), tooltip);
  gtk_accessible_update_property (GTK_ACCESSIBLE (GTK_WIDGET (remove)),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
  gtk_widget_add_css_class (GTK_WIDGET (remove), "flat");
  gtk_widget_set_valign (GTK_WIDGET (remove), GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (remove), "nuvasha-account-id",
                          g_strdup (account->id), g_free);
  g_object_set_data_full (G_OBJECT (remove), "nuvasha-account-name",
                          g_strdup (account->name), g_free);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_clicked), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (GTK_WIDGET (row)), GTK_WIDGET (remove));

  /* Which account a row opens travels with the row: the list is built again
     whenever the store changes, and a row is never asked about anything
     else. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (GTK_WIDGET (row)), TRUE);
  g_object_set_data_full (G_OBJECT (row), "nuvasha-account-id",
                          g_strdup (account->id), g_free);
  g_signal_connect (row, "activated", G_CALLBACK (on_row_activated), self);

  gtk_list_box_append (GTK_LIST_BOX (self->list_box), GTK_WIDGET (row));
}


/* ------------------------------------------------------ *
 * Adding one
 * ------------------------------------------------------ */

/* An account the user just signed in to, or typed a name for, goes into
   the store, which is what makes it the active one and writes it down. */
static void
add_account (NuvashaUsersPopover *self, NuvashaAccount *account)
{
  g_autoptr (GError) error = NULL;

  if (!nuvasha_accounts_add (self->accounts, account, &error))
    show_error (self, error->message);
  else
    nuvasha_ui_clear_error_label (self->error_label);
}

static void
on_account_added (AdwDialog *dialog, NuvashaAccount *account, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);

  add_account (self, account);
  adw_dialog_close (ADW_DIALOG (dialog));
  gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_add_microsoft_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  NuvashaSignInDialog *dialog = nuvasha_sign_in_dialog_new ();

  /* Weak, so that the handler does not keep the dialog alive for as long
     as it is connected to it. */
  g_signal_connect_object (dialog, "signed-in", G_CALLBACK (on_account_added),
                           self, 0);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
  gtk_popover_popdown (GTK_POPOVER (self));
}

static void
on_add_offline_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (user_data);
  NuvashaOfflineAccountDialog *dialog = nuvasha_offline_account_dialog_new ();

  g_signal_connect_object (dialog, "account-added",
                            G_CALLBACK (on_account_added), self, 0);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
  gtk_popover_popdown (GTK_POPOVER (self));
}


/* ------------------------------------------------------ *
 * The whole list
 * ------------------------------------------------------ */

/* What the store says, from scratch: every account in the order they were
   added, with the active one marked, and a sentence in place of the list
   when there are none. */
static void
rebuild (NuvashaUsersPopover *self)
{
  GPtrArray *accounts = nuvasha_accounts_list (self->accounts);
  GtkWidget *child;

  /* Whatever was still on its way belongs to rows that are about to be
     gone. */
  g_cancellable_cancel (self->avatars);
  g_clear_object (&self->avatars);
  self->avatars = g_cancellable_new ();

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->list_box))) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  /* With no accounts the list is not there at all: an empty boxed list
     still draws a blank card, and the scroller around it leaves a gap
     above the two buttons that add one. */
  gtk_widget_set_visible (GTK_WIDGET (self->list_scroller), accounts->len > 0);

  for (guint i = 0; i < accounts->len; i++)
    append_account_row (self, g_ptr_array_index (accounts, i));

  update_the_button (self);
}

static void
on_accounts_changed (NuvashaAccounts *accounts G_GNUC_UNUSED,
                     gpointer    user_data)
{
  rebuild (user_data);
}

static void
nuvasha_users_popover_init (NuvashaUsersPopover *self)
{
  GtkScrolledWindow *scrolled = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  GtkListBox *rows = GTK_LIST_BOX (gtk_list_box_new ());
  GtkButton *microsoft = GTK_BUTTON (gtk_button_new_with_label (_("Add Microsoft Account")));
  GtkButton *offline = GTK_BUTTON (gtk_button_new_with_label (_("Add Offline Account")));

  /* A row is chosen by clicking it, not by a selection the list keeps:
     the active account is the store's business. The list scrolls instead
     of the popover growing without end when there are many accounts. */
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (GTK_WIDGET (rows)), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (rows), "boxed-list");
  gtk_widget_set_size_request (GTK_WIDGET (scrolled), USERS_WIDTH, -1);
  gtk_scrolled_window_set_propagate_natural_width (
      GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (scrolled), GTK_ALIGN_CENTER);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (GTK_WIDGET (scrolled)),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (GTK_WIDGET (scrolled)), GTK_WIDGET (rows));
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (GTK_WIDGET (scrolled)),
                                              420);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_widget_set_size_request (GTK_WIDGET (rows), USERS_ROWS_WIDTH, -1);

  self->list_box = GTK_LIST_BOX (rows);
  self->list_scroller = GTK_SCROLLED_WINDOW (scrolled);

  self->error_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->error_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->error_label), "error");
  gtk_widget_set_margin_start (GTK_WIDGET (self->error_label), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->error_label), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->error_label), 6);
  gtk_widget_set_visible (GTK_WIDGET (self->error_label), FALSE);

  /* The two ways of having an account. The first one signs in without a
     password: the user types a code into a page of Microsoft's own. */
  gtk_widget_add_css_class (GTK_WIDGET (microsoft), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (offline), "flat");
  gtk_widget_set_halign (GTK_WIDGET (microsoft), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (offline), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (GTK_WIDGET (microsoft), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (offline), 12);
  g_signal_connect (microsoft, "clicked",
                    G_CALLBACK (on_add_microsoft_clicked), self);
  g_signal_connect (offline, "clicked", G_CALLBACK (on_add_offline_clicked),
                    self);

  GtkBox *content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_append (GTK_BOX (GTK_WIDGET (content)), GTK_WIDGET (scrolled));
  gtk_box_append (GTK_BOX (GTK_WIDGET (content)), GTK_WIDGET (microsoft));
  gtk_box_append (GTK_BOX (GTK_WIDGET (content)), GTK_WIDGET (offline));
  gtk_box_append (GTK_BOX (GTK_WIDGET (content)), GTK_WIDGET (self->error_label));

  gtk_popover_set_child (GTK_POPOVER (self), GTK_WIDGET (content));

  /* What the header says about who is playing: the avatar of the active
     account, or the default one when there is none. It is filled from the
     same store as the list, so the two cannot disagree. */
  self->avatar_image = GTK_IMAGE (gtk_image_new_from_icon_name ("avatar-default-symbolic"));
  gtk_image_set_pixel_size (GTK_IMAGE (self->avatar_image),
                            HEADER_AVATAR_SIZE);
  gtk_widget_set_valign (GTK_WIDGET (self->avatar_image), GTK_ALIGN_CENTER);

  self->button_content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  g_object_ref_sink (self->button_content);
  gtk_box_append (GTK_BOX (self->button_content), GTK_WIDGET (self->avatar_image));
}

static void
nuvasha_users_popover_constructed (GObject *object)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  /* Weak, so that the handler does not keep the store alive for as long
     as the popover is connected to it. */
  g_signal_connect_object (self->accounts, "changed",
                           G_CALLBACK (on_accounts_changed), self, 0);

  G_OBJECT_CLASS (nuvasha_users_popover_parent_class)->constructed (object);

  self->avatars = g_cancellable_new ();
  self->avatar_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              g_object_unref);
  rebuild (self);
}

static void
nuvasha_users_popover_dispose (GObject *object)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  /* An avatar that is still on its way is not needed by anything any
     more. */
  g_clear_object (&self->avatars);

  G_OBJECT_CLASS (nuvasha_users_popover_parent_class)->dispose (object);
}

static void
nuvasha_users_popover_finalize (GObject *object)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  g_clear_object (&self->accounts);
  g_clear_object (&self->button_content);
  g_clear_pointer (&self->avatar_cache, g_hash_table_unref);
  g_free (self->active_name);

  G_OBJECT_CLASS (nuvasha_users_popover_parent_class)->finalize (object);
}

enum {
  PROP_ACCOUNTS = 1,
  PROP_LAST
};

static void
nuvasha_users_popover_set_accounts (NuvashaUsersPopover *self, NuvashaAccounts *accounts);

static void
nuvasha_users_popover_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  switch (property_id)
    {
    case PROP_ACCOUNTS:
      g_value_set_object (value, self->accounts);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
nuvasha_users_popover_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  NuvashaUsersPopover *self = nuvasha_USERS_POPOVER (object);

  switch (property_id)
    {
    case PROP_ACCOUNTS:
      nuvasha_users_popover_set_accounts (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
nuvasha_users_popover_class_init (NuvashaUsersPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = nuvasha_users_popover_constructed;
  object_class->dispose = nuvasha_users_popover_dispose;
  object_class->finalize = nuvasha_users_popover_finalize;
  object_class->get_property = nuvasha_users_popover_get_property;
  object_class->set_property = nuvasha_users_popover_set_property;

  /* The store has to be there before the popover is built, because what
     is built is what the store holds. */
  g_object_class_install_property (
      object_class, PROP_ACCOUNTS,
      g_param_spec_object ("accounts", NULL, NULL, nuvasha_TYPE_ACCOUNTS,
                           G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
}

/* The store is taken by reference and never let go of before the
   popover is: it is what the whole widget is built from. */
static void
nuvasha_users_popover_set_accounts (NuvashaUsersPopover *self, NuvashaAccounts *accounts)
{
  g_set_object (&self->accounts, accounts);
}

NuvashaUsersPopover *
nuvasha_users_popover_new (NuvashaAccounts *accounts)
{
  g_return_val_if_fail (nuvasha_IS_ACCOUNTS (accounts), NULL);

  return g_object_new (nuvasha_TYPE_USERS_POPOVER, "accounts", accounts, NULL);
}

NuvashaAccounts *
nuvasha_users_popover_get_accounts (NuvashaUsersPopover *self)
{
  g_return_val_if_fail (nuvasha_IS_USERS_POPOVER (self), NULL);

  return self->accounts;
}

char *
nuvasha_users_popover_get_active_name (NuvashaUsersPopover *self)
{
  g_return_val_if_fail (nuvasha_IS_USERS_POPOVER (self), NULL);

  return g_strdup (self->active_name);
}

/* The avatar and the name in the header, which is what the menu button of
 * the popover shows. It is built here rather than in the init because it
 * belongs to the popover and is handed over by the window. */
GtkWidget *
nuvasha_users_popover_get_button_content (NuvashaUsersPopover *self)
{
  g_return_val_if_fail (nuvasha_IS_USERS_POPOVER (self), NULL);

  /* The same widget every time: a caller that asked twice is holding two
     references to one button, which is what it would want, and never two
     buttons over one avatar. */
  return GTK_WIDGET (g_object_ref (self->button_content));
}
