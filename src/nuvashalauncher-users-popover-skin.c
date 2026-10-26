/* Avatar loading and the account skin manager. */
#include "nuvashalauncher-users-popover-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"
#include <libsoup/soup.h>

#define AVATAR_SIZE 40

/* A skin is a picture of 64 by 64 and the face of it is 8 by 8 of that,
 * in the middle of the top of the picture. */
#define SKIN_SIZE 64
#define FACE_SIZE 8
#define FACE_X 8
#define FACE_Y 8

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

  skin = gdk_texture_new_from_file (file, &error);
  if (skin == NULL ||
      gdk_texture_get_width (skin) != 64 ||
      (gdk_texture_get_height (skin) != 32 && gdk_texture_get_height (skin) != 64))
    {
      g_clear_error (&error);
       error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    _("Skin images must be 64x64 or 64x32 PNG files"));
      goto done;
    }

  {
    g_autofree char *directory = skin_directory_path ();
    g_autofree char *source_basename = g_file_get_basename (file);
    g_autofree char *destination_basename = NULL;
    g_autofree char *destination_path = NULL;
    g_autoptr (GFile) destination = NULL;

    if (source_basename == NULL || !nuvashalauncher_fs_make_directory (directory, 0755, NULL))
      goto done;
    destination_basename = unique_skin_filename (directory, source_basename);
    destination_path = g_build_filename (directory, destination_basename, NULL);
    destination = g_file_new_for_path (destination_path);
    if (!nuvashalauncher_fs_copy_file (file, destination, G_FILE_COPY_OVERWRITE,
                           NULL, &error))
      goto done;
    g_clear_object (&file);
    file = g_steal_pointer (&destination);
  }

  /* Imported files are immediately usable as the row's preview. Uploading
   * is intentionally separate from choosing a local skin. */
  face = face_texture (skin);
  if (face != NULL)
    {
      gtk_image_set_from_paintable (menu->image, GDK_PAINTABLE (face));
      {
        char *uri = g_file_get_uri (file);
        g_autofree char *basename = g_file_get_basename (file);
        g_autofree char *display_name = skin_display_name (basename);
        AdwActionRow *row = skin_row_new (menu, display_name, uri);
        gtk_list_box_insert (menu->skins, GTK_WIDGET (row), 0);
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
  skin_menu_unref (menu);
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
                        on_skin_file_chosen, skin_menu_ref (source));
  g_object_unref (dialog);
}

static void
remove_skin_row (SkinMenu *menu, GtkListBoxRow *row)
{
  const char *url = g_object_get_data (G_OBJECT (row), "nuvashalauncher-skin-url");
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
    nuvashalauncher_fs_unlink (path, NULL);
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
    nuvashalauncher_users_set_avatar_icon (menu->image);
}

static void
on_remove_skin_confirmed (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinMenu *menu = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
  GtkListBoxRow *row = g_object_get_data (G_OBJECT (source), "nuvashalauncher-skin-row");

  if (g_str_equal (response, "remove") && row != NULL)
    {
      const char *url = g_object_get_data (G_OBJECT (row), "nuvashalauncher-skin-url");
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
      (url = g_object_get_data (G_OBJECT (row), "nuvashalauncher-skin-url")) == NULL ||
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
    g_object_set_data (G_OBJECT (alert), "nuvashalauncher-skin-row", row);
    adw_alert_dialog_choose (alert, GTK_WIDGET (menu->dialog), NULL,
                             on_remove_skin_confirmed, menu);
  }
}

static void
on_open_skins_clicked (GtkButton *button G_GNUC_UNUSED,
                       gpointer user_data G_GNUC_UNUSED)
{
  g_autofree char *directory =
    g_build_filename (g_get_user_data_dir (), "nuvashalauncher", "skins", NULL);
  g_autofree char *uri = NULL;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_fs_make_directory (directory, 0755, NULL))
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

  if (!nuvashalauncher_fs_copy_finish (G_FILE (source), result, &error))
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
    nuvashalauncher_fs_copy_async (source_file, destination, G_FILE_COPY_OVERWRITE,
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
  g_autoptr (GFile) source_file = NULL;
  g_autofree char *initial_name = NULL;

  if (row == NULL ||
      (url = g_object_get_data (G_OBJECT (row), "nuvashalauncher-skin-url")) == NULL)
    return;
  source_file = g_file_new_for_uri (url);
  initial_name = g_file_get_basename (source_file);
  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_initial_name (
    dialog, initial_name != NULL && *initial_name != '\0'
              ? initial_name : "skin.png");
  gtk_file_dialog_save (dialog,
                        GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (button))),
                        NULL, on_export_skin_chosen, g_object_ref (source_file));
  g_object_unref (dialog);
}

static void
on_skin_selected (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
  SkinMenu *menu = user_data;
  const char *url;

  if (row == NULL)
    return;
  url = g_object_get_data (G_OBJECT (row), "nuvashalauncher-skin-url");
  if (menu->remove_button != NULL)
    gtk_widget_set_sensitive (GTK_WIDGET (menu->remove_button),
                              url != NULL && !skin_url_is_default (url));

  if (url == NULL)
    return;

  /* Selecting a skin is the change action.  The profile service accepts the
   * URL of a skin already owned by the account; keep the row's avatar in sync
   * immediately while the service refresh happens on the next profile load. */
  g_object_set_data_full (G_OBJECT (menu->dialog), "nuvashalauncher-selected-skin",
                          g_strdup (url), g_free);
  gtk_list_box_select_row (list, row);
  nuvashalauncher_users_set_avatar_icon (menu->image);
  load_view_texture (menu->view, url, FALSE);

  /* Keep nuvashalauncher's cached profile in step with the selection so the account row
   * and the next opening of this manager show the selected skin. */
  if (!menu->applying_profile && menu->owner != NULL && menu->account != NULL)
    {
      g_autoptr (nuvashalauncherAccount) account = nuvashalauncher_account_copy (menu->account);
      g_autoptr (GError) error = NULL;
      g_free (account->avatar_url);
      account->avatar_url = g_strdup (url);
      g_free (account->skin_model);
      account->skin_model = g_strdup (menu->slim_model != NULL &&
                                      gtk_check_button_get_active (menu->slim_model)
                                        ? "SLIM" : "CLASSIC");
      nuvashalauncher_accounts_update (menu->owner->accounts, account, &error);
       upload_selected_skin (menu, url,
                             g_ascii_strcasecmp (account->skin_model, "SLIM") == 0
                               ? "SLIM" : "CLASSIC");
    }
}

AdwActionRow *
skin_row_new (SkinMenu *menu, const char *name, const char *url)
{
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-skin-row.ui");
  AdwActionRow *row = ADW_ACTION_ROW (g_object_ref (
    gtk_builder_get_object (builder, "row")));
  GtkImage *image = GTK_IMAGE (gtk_builder_get_object (builder, "avatar"));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 name != NULL && *name != '\0' ? name : _("Skin"));
  gtk_image_set_from_icon_name (image, "image-x-generic-symbolic");
  gtk_image_set_pixel_size (image, AVATAR_SIZE);
  nuvashalauncher_users_set_avatar_icon (image);
  if (url != NULL)
    {
      g_autofree char *identity = skin_name_for_url (url);
      g_hash_table_add (menu->skin_urls, g_strdup (identity));
      g_object_set_data_full (G_OBJECT (row), "nuvashalauncher-skin-url", g_strdup (url), g_free);
      if (g_str_has_prefix (url, "file://"))
        {
          g_autoptr (GFile) file = g_file_new_for_uri (url);
          g_autofree char *path = g_file_get_path (file);
          if (path == NULL || !nuvashalauncher_users_set_local_avatar (image, path))
            nuvashalauncher_users_set_avatar_icon (image);
        }
      else
        {
          g_autofree char *cached = cached_skin_path (url);
          if (cached == NULL || !nuvashalauncher_users_set_local_avatar (image, cached))
            nuvashalauncher_users_set_avatar (menu->owner, image, url);
        }
    }
  adw_action_row_add_prefix (row, GTK_WIDGET (image));
  return row;
}

void
select_saved_skin (SkinMenu *menu)
{
  GtkListBoxRow *match = NULL;
  GtkListBoxRow *row = NULL;
  GtkWidget *child = NULL;
  const char *saved_url = menu->account != NULL ? menu->account->avatar_url : NULL;

  for (child = gtk_widget_get_first_child (GTK_WIDGET (menu->skins));
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      const char *url = g_object_get_data (G_OBJECT (child), "nuvashalauncher-skin-url");

      if (saved_url != NULL && g_strcmp0 (saved_url, url) == 0)
        {
          match = GTK_LIST_BOX_ROW (child);
          break;
        }

      if (saved_url == NULL && url != NULL && skin_url_is_default (url) &&
          g_str_has_suffix (url, "/Steve.png"))
        row = GTK_LIST_BOX_ROW (child);
    }

  if (match == NULL)
    match = row;

  if (match != NULL)
    {
      menu->current_skin_row = ADW_ACTION_ROW (match);
      gtk_list_box_select_row (menu->skins, match);
    }
}

void
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
              /* The bundled Steve/Alex entries are already in the local
               * library. Never remove the currently selected local/imported
               * row merely because the profile also advertises a default. */
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

void
load_profile_skins (SkinMenu *menu)
{
  SoupMessage *message;
  const char *key;
  const char *cached;

  if (menu->account == NULL || menu->account->access_token == NULL)
    return;
  key = menu->account->uuid != NULL ? menu->account->uuid : menu->account->id;
  cached = g_hash_table_lookup (menu->owner->profile_cache, key);
  if (cached != NULL)
    {
      apply_profile_json (menu, cached);
      return;
    }
  message = nuvashalauncher_minecraft_services_message (
    SOUP_METHOD_GET, "/minecraft/profile", menu->account->access_token);
  nuvashalauncher_minecraft_services_send_async (message, menu->cancellable,
                                         on_capes_loaded, skin_menu_ref (menu));
  g_object_unref (message);
}

AdwDialog *
nuvashalauncher_users_skin_menu_new (nuvashalauncherUsersPopover *owner, GtkImage *image, nuvashalauncherAccount *account)
{
  nuvashalauncher_skin_view_get_type ();
  AdwDialog *dialog = adw_dialog_new ();
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  g_autoptr (GtkBuilder) content_builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-skin-content.ui");
  GtkBox *root = GTK_BOX (gtk_builder_get_object (content_builder, "root"));
  GtkBox *left = GTK_BOX (gtk_builder_get_object (content_builder, "left"));
  GtkBox *right = GTK_BOX (gtk_builder_get_object (content_builder, "right"));
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_builder_get_object (content_builder, "scroller"));
  GtkListBox *skins = GTK_LIST_BOX (gtk_builder_get_object (content_builder, "skins"));
  nuvashalauncherSkinView *preview = nuvashalauncher_SKIN_VIEW (gtk_builder_get_object (content_builder, "preview"));
  GtkCheckButton *default_model = GTK_CHECK_BUTTON (gtk_builder_get_object (content_builder, "classic-model"));
  GtkCheckButton *slim_model = GTK_CHECK_BUTTON (gtk_builder_get_object (content_builder, "slim-model"));
  GtkStringList *cape_list = gtk_string_list_new ((const char *[]) { _("No cape"), NULL });
  GtkDropDown *capes = GTK_DROP_DOWN (gtk_builder_get_object (content_builder, "capes"));
  GtkButton *import = GTK_BUTTON (gtk_builder_get_object (content_builder, "import"));
  GtkButton *export = GTK_BUTTON (gtk_builder_get_object (content_builder, "export"));
  GtkButton *open_skins = GTK_BUTTON (gtk_builder_get_object (content_builder, "open-skins"));
  GtkButton *remove = GTK_BUTTON (gtk_builder_get_object (content_builder, "remove"));
  gtk_drop_down_set_model (capes, G_LIST_MODEL (cape_list));
  AdwBreakpointCondition *narrow_condition =
    adw_breakpoint_condition_new_length (ADW_BREAKPOINT_CONDITION_MAX_WIDTH,
                                         700, ADW_LENGTH_UNIT_PX);
  g_autoptr (AdwBreakpoint) narrow_breakpoint =
    adw_breakpoint_new (narrow_condition);
  GValue vertical = G_VALUE_INIT;
  SkinMenu *data = g_new0 (SkinMenu, 1);

  data->image = g_object_ref (image);
  g_ref_count_init (&data->refs);
  data->view = g_object_ref (preview);
  data->owner = owner;
  data->dialog = dialog;
  data->skins = g_object_ref (skins);
  data->cape_list = g_object_ref (cape_list);
  data->cape_ids = g_ptr_array_new_with_free_func (g_free);
  data->cape_urls = g_ptr_array_new_with_free_func (g_free);
  data->skin_urls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  data->deleted_skin_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  data->capes = capes;
  data->account = account;
  data->remove_button = remove;
  data->default_model = default_model;
  data->slim_model = slim_model;
  data->cancellable = g_cancellable_new ();
  g_object_set_data (G_OBJECT (preview), "nuvashalauncher-deleted-skin-ids",
                     data->deleted_skin_ids);
  g_object_set_data (G_OBJECT (slim_model), "nuvashalauncher-slim", GINT_TO_POINTER (1));
  g_signal_connect (import, "clicked", G_CALLBACK (on_import_skin_clicked), data);
  g_signal_connect (export, "clicked", G_CALLBACK (on_export_skin_clicked), data);
  g_signal_connect (open_skins, "clicked", G_CALLBACK (on_open_skins_clicked), NULL);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_skin_clicked), data);
  g_signal_connect (skins, "row-selected", G_CALLBACK (on_skin_selected), data);
  g_signal_connect (default_model, "toggled", G_CALLBACK (on_model_toggled), data);
  g_signal_connect (slim_model, "toggled", G_CALLBACK (on_model_toggled), data);
  g_signal_connect (dialog, "close-attempt",
                    G_CALLBACK (on_skin_dialog_close_attempt), data);
  g_signal_connect (dialog, "closed",
                    G_CALLBACK (on_skin_dialog_closed), data);

  gtk_widget_set_size_request (GTK_WIDGET (left), 220, -1);
  gtk_widget_set_hexpand (GTK_WIDGET (right), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (right), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (scroller), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (skins), "boxed-list");
  gtk_widget_add_css_class (GTK_WIDGET (skins), "nuvashalauncher-list-section");
  gtk_widget_set_halign (GTK_WIDGET (preview), GTK_ALIGN_FILL);
  gtk_widget_set_vexpand (GTK_WIDGET (preview), TRUE);
  gtk_widget_set_size_request (GTK_WIDGET (preview), 220, 300);
  gtk_check_button_set_group (slim_model, default_model);
  data->applying_profile = TRUE;
  gtk_check_button_set_active (default_model,
                               g_ascii_strcasecmp (account->skin_model != NULL
                                                     ? account->skin_model : "CLASSIC",
                                                   "SLIM") != 0);
  gtk_check_button_set_active (slim_model,
                               g_ascii_strcasecmp (account->skin_model != NULL
                                                     ? account->skin_model : "CLASSIC",
                                                   "SLIM") == 0);
  nuvashalauncher_skin_view_set_slim (preview,
                              g_ascii_strcasecmp (account->skin_model != NULL
                                                    ? account->skin_model : "CLASSIC",
                                                  "SLIM") == 0);
  data->applying_profile = FALSE;
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (open_skins), _("Open Folder"));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (remove), _("Delete"));
  gtk_widget_add_css_class (GTK_WIDGET (remove), "destructive-action");
  gtk_widget_set_hexpand (GTK_WIDGET (export), FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (import), FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (remove), FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (open_skins), FALSE);
  gtk_widget_set_halign (GTK_WIDGET (remove), GTK_ALIGN_END);
  gtk_accessible_update_property (GTK_ACCESSIBLE (open_skins),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Open Folder"), -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (remove),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Delete"), -1);
  gtk_widget_set_size_request (GTK_WIDGET (root), 420, 400);
  g_value_init (&vertical, GTK_TYPE_ORIENTATION);
  g_value_set_enum (&vertical, GTK_ORIENTATION_VERTICAL);
  adw_breakpoint_add_setter (narrow_breakpoint, G_OBJECT (root),
                             "orientation", &vertical);
  g_value_unset (&vertical);
  adw_dialog_set_title (dialog, _("Skin"));
  adw_dialog_set_content_width (dialog, 860);
  adw_dialog_set_content_height (dialog, 560);
  gtk_widget_set_size_request (GTK_WIDGET (dialog), 420, 400);
  adw_dialog_add_breakpoint (dialog, g_steal_pointer (&narrow_breakpoint));
  adw_header_bar_set_show_back_button (header, FALSE);
  adw_toolbar_view_add_top_bar (toolbar, GTK_WIDGET (header));
  adw_toolbar_view_set_content (toolbar, GTK_WIDGET (root));
  adw_dialog_set_child (dialog, GTK_WIDGET (toolbar));
  append_local_skins (data);
  append_default_skins (data);
  if (account->avatar_url != NULL && *account->avatar_url != '\0')
    {
      if (!g_hash_table_contains (data->skin_urls,
                                  skin_name_for_url (account->avatar_url)) &&
          !g_hash_table_contains (data->deleted_skin_ids,
                                  skin_name_for_url (account->avatar_url)))
        {
          g_autofree char *skin_name = skin_name_for_url (account->avatar_url);
          if (!g_hash_table_contains (data->skin_urls, skin_name))
            {
              data->current_skin_row = skin_row_new (data, skin_name,
                                                     account->avatar_url);
              gtk_list_box_append (skins, GTK_WIDGET (data->current_skin_row));
            }
          load_view_texture (preview, account->avatar_url, FALSE);
        }
    }
  else
    {
      data->applying_profile = TRUE;
      select_saved_skin (data);
      data->applying_profile = FALSE;
    }
  g_signal_connect (capes, "notify::selected", G_CALLBACK (on_cape_selected), data);
  load_profile_skins (data);
  return dialog;
}
