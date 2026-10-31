/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-users-popover-private.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"
#include <libsoup/soup.h>

static void schedule_pending_skin_upload (SkinMenu *, guint);
static void start_pending_skin_upload (SkinMenu *);
static void send_pending_cape_change (SkinMenu *);
static void change_cape_on_server (SkinMenu *, const char *);

typedef struct {
  SkinMenu *menu;
  char *url;
  char *variant;
  GCancellable *cancellable;
  SoupMessage *message;
  gboolean background_counted;
} SkinUpload;

static guint skin_background_operations;
static SoupSession *skin_upload_session;

gboolean
nuvashalauncher_users_skin_background_work_active (void)
{
  return skin_background_operations > 0;
}

static void
skin_upload_free (SkinUpload *upload)
{
  SkinMenu *menu = upload->menu;

  g_free (upload->url);
  g_free (upload->variant);
  g_clear_object (&upload->cancellable);
  g_clear_object (&upload->message);
  g_free (upload);
  skin_menu_unref (menu);
}

static void
skin_upload_background_finish (SkinUpload *upload)
{
  if (upload != NULL && upload->background_counted)
    {
      upload->background_counted = FALSE;
      if (skin_background_operations > 0)
        skin_background_operations--;
    }
}


SkinMenu *
skin_menu_ref (SkinMenu *menu)
{
  if (menu != NULL)
    g_ref_count_inc (&menu->refs);
  return menu;
}

static void
skin_menu_free (SkinMenu *menu)
{
  if (menu == NULL)
    return;

  if (menu->skin_upload_retry_source != 0)
    g_source_remove (menu->skin_upload_retry_source);
  g_clear_object (&menu->skin_upload_cancellable);
  g_clear_object (&menu->cancellable);
  g_clear_pointer (&menu->skin_upload_pending_url, g_free);
  g_clear_pointer (&menu->skin_upload_pending_variant, g_free);
  g_clear_pointer (&menu->pending_cape_id, g_free);
  g_clear_pointer (&menu->cape_ids, g_ptr_array_unref);
  g_clear_pointer (&menu->cape_urls, g_ptr_array_unref);
  g_clear_pointer (&menu->skin_urls, g_hash_table_unref);
  g_clear_pointer (&menu->deleted_skin_ids, g_hash_table_unref);
  g_clear_object (&menu->cape_list);
  g_clear_object (&menu->skins);
  g_clear_object (&menu->view);
  g_clear_object (&menu->image);
  g_clear_object (&menu->operation_status);
  g_free (menu);
}

void
skin_menu_unref (SkinMenu *menu)
{
  if (menu != NULL && g_ref_count_dec (&menu->refs))
    skin_menu_free (menu);
}


#define SKIN_SIZE 64
#define FACE_SIZE 8
#define FACE_X 8
#define FACE_Y 8

void
skin_menu_show_error (SkinMenu *menu, const GError *error)
{
  if (menu == NULL || nuvashalauncher_error_is_cancelled (error))
    return;
  if (!menu->closed && menu->operation_status != NULL)
    {
      nuvashalauncher_ui_error_label (menu->operation_status, error);
      return;
    }
  /* Automatic retries remain visible without repeatedly opening dialogs. */
  if (g_error_matches (error, nuvashalauncher_MINECRAFT_SERVICES_ERROR,
                        nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED))
    return;
  nuvashalauncher_ui_show_error (menu->closed ? NULL : GTK_WIDGET (menu->dialog),
                                _("Minecraft Services error"), error);
}

void
on_skin_dialog_close_attempt (AdwDialog *dialog G_GNUC_UNUSED, gpointer user_data)
{
  SkinMenu *menu = user_data;

  menu->closed = TRUE;
}

void
on_skin_dialog_closed (AdwDialog *dialog G_GNUC_UNUSED, gpointer user_data)
{
  SkinMenu *menu = user_data;

  /* Profile/cape requests belong to the dialog and can stop here. Skin
   * uploads deliberately use their own cancellable and retain the menu
  * context until they finish. */
  menu->closed = TRUE;
  g_cancellable_cancel (menu->cancellable);
  skin_menu_unref (menu);
}

static void
save_cape_choice (SkinMenu *menu)
{
  g_autoptr (nuvashalauncherAccount) account = NULL;
  g_autoptr (GError) error = NULL;
  if (menu->owner == NULL || menu->account == NULL)
    return;
  account = nuvashalauncher_account_copy (menu->account);
  if (!nuvashalauncher_accounts_update (menu->owner->accounts, account, &error) && error != NULL)
    skin_menu_show_error (menu, error);
}

static void
on_skin_upload_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  SkinUpload *upload = user_data;
  SkinMenu *menu = upload != NULL ? skin_menu_ref (upload->menu) : NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) response =
    g_task_propagate_pointer (G_TASK (result), &error);
  gboolean rate_limited = error != NULL &&
                          error->domain == nuvashalauncher_MINECRAFT_SERVICES_ERROR &&
                          error->code == nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED;

  if (menu != NULL && (response == NULL || error != NULL) && rate_limited)
    {
      guint delay = nuvashalauncher_minecraft_services_retry_delay (
        NULL, menu->skin_upload_retry_attempt++);

      /* Keep the failed selection unless the user has already selected a
       * newer one. The next attempt always uses the latest pending URL. */
      if (menu->skin_upload_pending_url == NULL && upload != NULL)
        menu->skin_upload_pending_url = g_strdup (upload->url);
      menu->skin_upload_retry_after = g_get_real_time () / 1000000 + delay;
      schedule_pending_skin_upload (menu, delay);
      skin_menu_show_error (menu, error);
    }
  else if (menu != NULL && (response == NULL || error != NULL))
    skin_menu_show_error (menu, error);

  if (menu != NULL)
    {
      menu->skin_upload_in_flight = FALSE;
      g_clear_object (&menu->skin_upload_cancellable);
      if (response != NULL && error == NULL)
        {
          menu->skin_upload_retry_after = 0;
          menu->skin_upload_retry_attempt = 0;
          if (!menu->closed && menu->operation_status != NULL)
            nuvashalauncher_ui_clear_error_label (menu->operation_status);
        }
    }
  if (!rate_limited)
    skin_upload_background_finish (upload);
  if (menu != NULL && !rate_limited)
    {
      gboolean succeeded = response != NULL && error == NULL;

      start_pending_skin_upload (menu);
      if (succeeded && !menu->skin_upload_in_flight)
        send_pending_cape_change (menu);
    }

  if (upload != NULL)
    skin_upload_free (upload);
  skin_menu_unref (menu);
}

static void
send_skin_upload (SkinUpload *upload, GBytes *bytes)
{
  SoupMessage *message = nuvashalauncher_minecraft_services_message (
    SOUP_METHOD_POST, "/minecraft/profile/skins",
    upload->menu->account->access_token);
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GBytes) prepared = NULL;
  g_autofree char *content_type = NULL;
  prepared = g_ascii_strcasecmp (upload->variant, "CLASSIC") == 0
    ? nuvashalauncher_skin_prepare_classic_upload (bytes) : g_bytes_ref (bytes);
  body = nuvashalauncher_minecraft_services_skin_multipart (prepared, upload->variant,
                                                      &content_type);
  soup_message_set_request_body_from_bytes (message, content_type, body);
  nuvashalauncher_minecraft_services_send_async (message, upload->cancellable,
                                         on_skin_upload_done, upload);
  g_object_unref (message);
}

static void
on_skin_download_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinUpload *upload = user_data;
  SkinMenu *menu = skin_menu_ref (upload->menu);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (
    SOUP_SESSION (source), result, &error);

  if (upload->message != NULL &&
      !SOUP_STATUS_IS_SUCCESSFUL (soup_message_get_status (upload->message)))
    {
      g_clear_pointer (&bytes, g_bytes_unref);
      g_clear_error (&error);
      g_set_error (&error, nuvashalauncher_MINECRAFT_SERVICES_ERROR,
                   nuvashalauncher_MINECRAFT_SERVICES_ERROR_HTTP,
                   "skin request returned HTTP status %u",
                   soup_message_get_status (upload->message));
    }
  if (bytes == NULL)
    {
      skin_menu_show_error (menu, error);
      upload->menu->skin_upload_in_flight = FALSE;
      g_clear_object (&upload->menu->skin_upload_cancellable);
      skin_upload_background_finish (upload);
      skin_upload_free (upload);
      start_pending_skin_upload (menu);
      skin_menu_unref (menu);
      return;
    }
  send_skin_upload (upload, bytes);
  skin_menu_unref (menu);
}

static void
on_skin_upload_file_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SkinUpload *upload = user_data;
  SkinMenu *menu = skin_menu_ref (upload->menu);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);
  if (bytes == NULL)
    {
      skin_menu_show_error (menu, error);
      menu->skin_upload_in_flight = FALSE;
      g_clear_object (&menu->skin_upload_cancellable);
      skin_upload_background_finish (upload);
      skin_upload_free (upload);
      start_pending_skin_upload (menu);
      skin_menu_unref (menu);
      return;
    }
  if (menu->skin_upload_pending_url != NULL)
    {
      menu->skin_upload_in_flight = FALSE;
      g_clear_object (&menu->skin_upload_cancellable);
      skin_upload_background_finish (upload);
      skin_upload_free (upload);
      start_pending_skin_upload (menu);
      skin_menu_unref (menu);
      return;
    }
  send_skin_upload (upload, bytes);
  skin_menu_unref (menu);
}

static void
start_pending_skin_upload (SkinMenu *menu)
{
  g_autoptr (GFile) file = NULL;
  SkinUpload *upload;
  gint64 now = g_get_real_time () / 1000000;

  if (menu == NULL || menu->skin_upload_pending_url == NULL ||
      menu->account == NULL || menu->account->access_token == NULL ||
      !nuvashalauncher_account_token_is_valid (menu->account, g_get_real_time () / 1000000) ||
      menu->skin_upload_in_flight)
    return;

  if (menu->skin_upload_retry_after > now)
    {
      guint delay = (guint) (menu->skin_upload_retry_after - now);

      if (menu->skin_upload_retry_source == 0)
        schedule_pending_skin_upload (menu, delay);
      return;
    }

  if (menu->skin_upload_retry_source != 0)
    {
      g_source_remove (menu->skin_upload_retry_source);
      menu->skin_upload_retry_source = 0;
    }

  {
    g_autofree char *url = g_steal_pointer (&menu->skin_upload_pending_url);
    g_autofree char *variant = g_steal_pointer (&menu->skin_upload_pending_variant);

    if (variant == NULL || *variant == '\0')
      variant = g_strdup ("CLASSIC");

    if (!g_str_has_prefix (url, "file://") &&
        !g_str_has_prefix (url, "http://") &&
        !g_str_has_prefix (url, "https://"))
      return;

    menu->skin_upload_in_flight = TRUE;
    if (!g_str_has_prefix (url, "file://"))
      {
        upload = g_new0 (SkinUpload, 1);
        upload->menu = skin_menu_ref (menu);
        upload->url = g_steal_pointer (&url);
        upload->variant = g_steal_pointer (&variant);
        upload->cancellable = g_cancellable_new ();
        upload->background_counted = TRUE;
        skin_background_operations++;
        menu->skin_upload_cancellable = g_object_ref (upload->cancellable);
        if (skin_upload_session == NULL)
          skin_upload_session = soup_session_new ();
        upload->message = soup_message_new (SOUP_METHOD_GET, upload->url);
        soup_session_send_and_read_async (skin_upload_session, upload->message,
                                          G_PRIORITY_DEFAULT, upload->cancellable,
                                          on_skin_download_done, upload);
        return;
      }

    file = g_file_new_for_uri (url);
    upload = g_new0 (SkinUpload, 1);
    upload->menu = skin_menu_ref (menu);
    upload->url = g_steal_pointer (&url);
    upload->variant = g_steal_pointer (&variant);
    upload->cancellable = g_cancellable_new ();
    upload->background_counted = TRUE;
    skin_background_operations++;
    menu->skin_upload_cancellable = g_object_ref (upload->cancellable);
    g_file_load_bytes_async (file, upload->cancellable,
                             on_skin_upload_file_loaded, upload);
  }
}

static gboolean
retry_pending_skin_upload (gpointer user_data)
{
  SkinMenu *menu = user_data;

  menu->skin_upload_retry_source = 0;
  menu->skin_upload_retry_after = 0;
  start_pending_skin_upload (menu);
  return G_SOURCE_REMOVE;
}

void
upload_selected_skin (SkinMenu *menu, const char *url, const char *variant)
{
  if (menu == NULL || url == NULL ||
      (!g_str_has_prefix (url, "file://") &&
       !g_str_has_prefix (url, "http://") &&
       !g_str_has_prefix (url, "https://")))
    return;

  if (menu->skin_upload_in_flight && menu->skin_upload_cancellable != NULL)
    g_cancellable_cancel (menu->skin_upload_cancellable);
  g_free (menu->skin_upload_pending_url);
  menu->skin_upload_pending_url = g_strdup (url);
  g_free (menu->skin_upload_pending_variant);
  menu->skin_upload_pending_variant = g_strdup (
    variant != NULL && g_ascii_strcasecmp (variant, "SLIM") == 0
      ? "SLIM" : "CLASSIC");

  if (!menu->skin_upload_in_flight)
    start_pending_skin_upload (menu);
}

static void
schedule_pending_skin_upload (SkinMenu *menu, guint delay)
{
  if (menu->skin_upload_retry_source != 0)
    g_source_remove (menu->skin_upload_retry_source);
  menu->skin_upload_retry_source = g_timeout_add_seconds_full (
    G_PRIORITY_DEFAULT, MAX (1u, delay), retry_pending_skin_upload,
    skin_menu_ref (menu), (GDestroyNotify) skin_menu_unref);
}

void
on_capes_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  SkinMenu *menu = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_task_propagate_pointer (G_TASK (result), &error);

  if (menu->closed)
    {
      if (bytes != NULL && error == NULL)
        {
          gsize length = 0;
          const char *data = g_bytes_get_data (bytes, &length);
          g_hash_table_replace (menu->owner->profile_cache,
                                g_strdup (menu->account->uuid != NULL
                                            ? menu->account->uuid : menu->account->id),
                                g_strndup (data, length));
        }
      else
        g_hash_table_replace (menu->owner->profile_cache,
                              g_strdup (menu->account->uuid != NULL
                                          ? menu->account->uuid : menu->account->id),
                              g_strdup (""));
      skin_menu_unref (menu);
      return;
    }

  if (bytes == NULL || error != NULL)
    {
      skin_menu_show_error (menu, error);
      g_hash_table_replace (menu->owner->profile_cache,
                            g_strdup (menu->account->uuid != NULL
                                        ? menu->account->uuid : menu->account->id),
                            g_strdup (""));
      goto done;
    }

  {
    gsize length = 0;
    const char *data = g_bytes_get_data (bytes, &length);
    g_autofree char *json = g_strndup (data, length);
    g_hash_table_replace (menu->owner->profile_cache,
                          g_strdup (menu->account->uuid != NULL
                                      ? menu->account->uuid : menu->account->id),
                          g_strdup (json));
    apply_profile_json (menu, json);
  }

done:
  menu->applying_profile = FALSE;
  append_default_skins (menu);
  select_saved_skin (menu);
  g_object_set_data (G_OBJECT (menu->dialog), "nuvashalauncher-capes-loaded",
                     GINT_TO_POINTER (1));
  skin_menu_unref (menu);
}

static void
clear_profile_capes (SkinMenu *menu)
{
  while (g_list_model_get_n_items (G_LIST_MODEL (menu->cape_list)) > 1)
    gtk_string_list_remove (menu->cape_list,
                            g_list_model_get_n_items (G_LIST_MODEL (menu->cape_list)) - 1);
  g_ptr_array_set_size (menu->cape_ids, 0);
  g_ptr_array_set_size (menu->cape_urls, 0);
  gtk_drop_down_set_selected (menu->capes, 0);
}

void
apply_profile_json (SkinMenu *menu, const char *json)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonNode *node;
  JsonArray *capes;

  if (!json_parser_load_from_data (parser, json, -1, NULL))
    return;
  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root))
    return;
  node = json_object_get_member (json_node_get_object (root), "capes");
  if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
    return;
  capes = json_node_get_array (node);

  clear_profile_capes (menu);
  append_profile_skins (menu, json);
  menu->applying_profile = TRUE;
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
      if ((state != NULL && g_str_equal (state, "ACTIVE")) ||
          (menu->account->cape_id != NULL &&
           g_str_equal (menu->account->cape_id, id)))
        {
          gtk_drop_down_set_selected (menu->capes, menu->cape_ids->len);
          if (url != NULL && *url != '\0')
            load_view_texture (menu->view, url, TRUE);
        }
    }
  menu->applying_profile = FALSE;
  append_default_skins (menu);
  select_saved_skin (menu);
  g_object_set_data (G_OBJECT (menu->dialog), "nuvashalauncher-capes-loaded",
                     GINT_TO_POINTER (1));
}

void
on_cape_selected (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                  gpointer user_data)
{
  SkinMenu *menu = user_data;
  guint selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (object));

  if (menu->applying_profile)
    return;

  if (selected == 0 || selected - 1 >= menu->cape_urls->len)
    {
      nuvashalauncher_skin_view_set_cape (menu->view, NULL);
      g_free (menu->account->cape_id);
      menu->account->cape_id = NULL;
      save_cape_choice (menu);
      change_cape_on_server (menu, NULL);
      return;
    }
  load_view_texture (menu->view,
                     g_ptr_array_index (menu->cape_urls, selected - 1), TRUE);
  g_free (menu->account->cape_id);
  menu->account->cape_id = g_strdup (g_ptr_array_index (menu->cape_ids, selected - 1));
  save_cape_choice (menu);
  change_cape_on_server (menu, menu->account->cape_id);
}

static void
change_cape_on_server (SkinMenu *menu, const char *cape_id)
{
  g_free (menu->pending_cape_id);
  menu->pending_cape_id = g_strdup (cape_id != NULL ? cape_id : "");

  if (menu->skin_upload_in_flight)
    return;

  send_pending_cape_change (menu);
}

static void
send_pending_cape_change (SkinMenu *menu)
{
  SoupMessage *message;
  g_autofree char *json = NULL;
  g_autofree char *cape_id = NULL;

  if (menu->account == NULL || menu->account->access_token == NULL ||
      !nuvashalauncher_account_token_is_valid (menu->account, g_get_real_time () / 1000000))
    return;
  cape_id = g_steal_pointer (&menu->pending_cape_id);
  message = nuvashalauncher_minecraft_services_message (
    cape_id != NULL && *cape_id != '\0' ? SOUP_METHOD_PUT : SOUP_METHOD_DELETE,
    "/minecraft/profile/capes/active", menu->account->access_token);
  if (cape_id != NULL && *cape_id != '\0')
    {
      json = g_strdup_printf ("{\"capeId\":\"%s\"}", cape_id);
      soup_message_set_request_body_from_bytes (message, "application/json",
                                                g_bytes_new (json, strlen (json)));
    }
  skin_background_operations++;
  nuvashalauncher_minecraft_services_send_async (message, menu->cancellable,
                                          on_active_skin_delete_done,
                                          skin_menu_ref (menu));
  g_object_unref (message);
}

void
on_model_toggled (GtkCheckButton *button, gpointer user_data)
{
  SkinMenu *menu = user_data;
  if (gtk_check_button_get_active (button) && !menu->applying_profile)
    nuvashalauncher_skin_view_set_slim (menu->view,
                            g_object_get_data (G_OBJECT (button), "nuvashalauncher-slim") != NULL);

  if (gtk_check_button_get_active (button) && !menu->applying_profile &&
      menu->account != NULL && menu->owner != NULL)
    {
      g_autoptr (nuvashalauncherAccount) account = nuvashalauncher_account_copy (menu->account);
      g_autoptr (GError) error = NULL;
      g_free (account->skin_model);
      account->skin_model = g_strdup (menu->slim_model == button ? "SLIM" : "CLASSIC");
      if (!nuvashalauncher_accounts_update (menu->owner->accounts, account, &error) && error != NULL)
        skin_menu_show_error (menu, error);
      {
        GtkListBoxRow *row = gtk_list_box_get_selected_row (menu->skins);
        const char *url = row != NULL
          ? g_object_get_data (G_OBJECT (row), "nuvashalauncher-skin-url") : NULL;
        upload_selected_skin (menu, url,
                              menu->slim_model == button ? "SLIM" : "CLASSIC");
      }
    }
}



void
on_active_skin_delete_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                            gpointer user_data G_GNUC_UNUSED)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_task_propagate_pointer (G_TASK (result), &error);
  SkinMenu *menu = user_data;

  if (skin_background_operations > 0)
    skin_background_operations--;
  if (menu->closed)
    {
      skin_menu_unref (menu);
      return;
    }

  if (bytes == NULL || error != NULL)
    {
      if (error != NULL &&
          error->domain == nuvashalauncher_MINECRAFT_SERVICES_ERROR &&
          error->code == nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED)
        {
          skin_menu_unref (menu);
          return;
        }
      g_warning ("%s", nuvashalauncher_error_message (error));
      skin_menu_show_error (menu, error);
    }
  skin_menu_unref (menu);
}

void
delete_active_skin (SkinMenu *menu)
{
  SoupMessage *message;

  if (menu->account == NULL || menu->account->type != nuvashalauncher_ACCOUNT_MICROSOFT ||
      menu->account->access_token == NULL)
    return;
  message = nuvashalauncher_minecraft_services_message (
    SOUP_METHOD_DELETE, "/minecraft/profile/skins/active",
    menu->account->access_token);
  skin_background_operations++;
  nuvashalauncher_minecraft_services_send_async (message, menu->cancellable,
                                         on_active_skin_delete_done,
                                         skin_menu_ref (menu));
  g_object_unref (message);
}
