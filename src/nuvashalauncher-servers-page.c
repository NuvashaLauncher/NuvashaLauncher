/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-servers-page.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-nbt.h"
#include "nuvashalauncher-ui-feedback.h"

typedef struct {
  char *name;
  char *ip;
  char *icon_b64;
  gint8 accept_textures; /* 0=ask, 1=always, 2=never */
  gboolean has_accept_textures;
  gint8 hidden;
  gboolean has_hidden;
} ServerEntry;

static void
server_entry_free (ServerEntry *s)
{
  if (s == NULL)
    return;
  g_free (s->name);
  g_free (s->ip);
  g_free (s->icon_b64);
  g_free (s);
}

struct _NuvashaLauncherServersPage {
  GtkBox parent_instance;

  NuvashaLauncherInstance *instance;
  NuvashaLauncher *launcher;
  char *servers_dat_path;

  AdwToastOverlay *toasts;
  GtkBox *bar;
  GtkWidget *add_button;
  GtkWidget *refresh_button;
  AdwViewStack *stack;
  AdwPreferencesGroup *group;
  GtkSearchEntry *search_bar;
  AdwHeaderBar *header_bar;
  AdwClamp *header_search_clamp;
  GPtrArray *rows;
  GPtrArray *servers;
};

G_DEFINE_FINAL_TYPE (NuvashaLauncherServersPage, nuvashalauncher_servers_page, GTK_TYPE_BOX)

static gboolean
save_servers_dat (NuvashaLauncherServersPage *self, GError **error)
{
  if (self->servers_dat_path == NULL)
    return FALSE;

  g_autofree char *dir = g_path_get_dirname (self->servers_dat_path);
  g_mkdir_with_parents (dir, 0755);

  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "");
  NuvashaLauncherNbtTag *list =
    nuvashalauncher_nbt_new_list ("servers", NUVASHALAUNCHER_NBT_COMPOUND);

  for (guint i = 0; i < self->servers->len; i++)
    {
      ServerEntry *s = g_ptr_array_index (self->servers, i);
      NuvashaLauncherNbtTag *c =
        nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, NULL);

      nuvashalauncher_nbt_compound_set (
        c, nuvashalauncher_nbt_new_string ("name", s->name));
      nuvashalauncher_nbt_compound_set (
        c, nuvashalauncher_nbt_new_string ("ip", s->ip));

      if (s->has_accept_textures || s->accept_textures != 0)
        nuvashalauncher_nbt_compound_set (
          c, nuvashalauncher_nbt_new_byte ("acceptTextures", s->accept_textures));
      if (s->icon_b64 != NULL && *s->icon_b64 != '\0')
        nuvashalauncher_nbt_compound_set (
          c, nuvashalauncher_nbt_new_string ("icon", s->icon_b64));
      if (s->has_hidden)
        nuvashalauncher_nbt_compound_set (
          c, nuvashalauncher_nbt_new_byte ("hidden", s->hidden));

      g_ptr_array_add (list->v_list.items, c);
    }

  nuvashalauncher_nbt_compound_set (root, list);
  return nuvashalauncher_nbt_write_file (root, self->servers_dat_path, FALSE, error);
}

static void
persist_and_reload (NuvashaLauncherServersPage *self)
{
  g_autoptr (GError) error = NULL;
  if (!save_servers_dat (self, &error))
    nuvashalauncher_ui_error_toast (GTK_WIDGET (self), GTK_WIDGET (self->toasts), error);
  nuvashalauncher_servers_page_reload (self);
}

static void
on_join_server_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherServersPage *self = NUVASHALAUNCHER_SERVERS_PAGE (user_data);
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "nuvashalauncher-idx"));

  if (self->launcher == NULL || index >= self->servers->len)
    return;

  ServerEntry *s = g_ptr_array_index (self->servers, index);
  if (s->ip == NULL || *s->ip == '\0')
    return;

  nuvashalauncher_launcher_set_quick_play (self->launcher, NULL, s->ip);
  nuvashalauncher_launcher_start (self->launcher);
}

static void
on_move_up_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherServersPage *self = NUVASHALAUNCHER_SERVERS_PAGE (user_data);
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "nuvashalauncher-idx"));

  if (index == 0 || index >= self->servers->len)
    return;

  gpointer a = g_ptr_array_index (self->servers, index - 1);
  gpointer b = g_ptr_array_index (self->servers, index);
  g_ptr_array_index (self->servers, index - 1) = b;
  g_ptr_array_index (self->servers, index) = a;
  persist_and_reload (self);
}

static void
on_move_down_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherServersPage *self = NUVASHALAUNCHER_SERVERS_PAGE (user_data);
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "nuvashalauncher-idx"));

  if (index + 1 >= self->servers->len)
    return;

  gpointer a = g_ptr_array_index (self->servers, index);
  gpointer b = g_ptr_array_index (self->servers, index + 1);
  g_ptr_array_index (self->servers, index) = b;
  g_ptr_array_index (self->servers, index + 1) = a;
  persist_and_reload (self);
}

typedef struct {
  NuvashaLauncherServersPage *page;
  gboolean is_edit;
  guint index;
  AdwEntryRow *name_row;
  AdwEntryRow *ip_row;
  AdwComboRow *policy_row;
} ServerDialogData;

static void
on_server_dialog_response (AdwAlertDialog *dialog G_GNUC_UNUSED,
                           const char *response,
                           gpointer user_data)
{
  ServerDialogData *d = user_data;
  if (g_strcmp0 (response, "save") != 0)
    {
      g_free (d);
      return;
    }

  const char *name = gtk_editable_get_text (GTK_EDITABLE (d->name_row));
  const char *ip = gtk_editable_get_text (GTK_EDITABLE (d->ip_row));
  guint policy_idx = adw_combo_row_get_selected (d->policy_row);
  gint8 policy = policy_idx == 1 ? 1 : (policy_idx == 2 ? 2 : 0);

  if (ip == NULL || *ip == '\0')
    {
      g_free (d);
      return;
    }

  if (d->is_edit)
    {
      if (d->index < d->page->servers->len)
        {
          ServerEntry *s = g_ptr_array_index (d->page->servers, d->index);
          g_free (s->name);
          s->name = g_strdup (name != NULL && *name != '\0' ? name : ip);
          g_free (s->ip);
          s->ip = g_strdup (ip);
          s->accept_textures = policy;
          s->has_accept_textures = TRUE;
        }
    }
  else
    {
      ServerEntry *s = g_new0 (ServerEntry, 1);
      s->name = g_strdup (name != NULL && *name != '\0' ? name : ip);
      s->ip = g_strdup (ip);
      s->accept_textures = policy;
      s->has_accept_textures = TRUE;
      g_ptr_array_add (d->page->servers, s);
    }

  persist_and_reload (d->page);
  g_free (d);
}

static void
present_server_dialog (NuvashaLauncherServersPage *self,
                       gboolean is_edit,
                       guint index)
{
  ServerEntry *existing =
    (is_edit && index < self->servers->len)
      ? g_ptr_array_index (self->servers, index)
      : NULL;

  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (is_edit ? _("Edit Server") : _("Add Server"), NULL));

  AdwPreferencesGroup *form = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwEntryRow *name_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_row), _("Name"));
  gtk_editable_set_text (GTK_EDITABLE (name_row),
                         existing != NULL && existing->name != NULL ? existing->name : "");

  AdwEntryRow *ip_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ip_row), _("Address"));
  gtk_editable_set_text (GTK_EDITABLE (ip_row),
                         existing != NULL && existing->ip != NULL ? existing->ip : "");

  const char *policies[] = {
    _("Ask to download"),
    _("Always download"),
    _("Never download"),
    NULL
  };
  GtkStringList *model = gtk_string_list_new (policies);
  AdwComboRow *policy_row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (policy_row), _("Resource Packs"));
  adw_combo_row_set_model (policy_row, G_LIST_MODEL (model));
  g_object_unref (model);

  guint sel = 0;
  if (existing != NULL && existing->has_accept_textures)
    sel = existing->accept_textures == 1 ? 1 : (existing->accept_textures == 2 ? 2 : 0);
  adw_combo_row_set_selected (policy_row, sel);

  adw_preferences_group_add (form, GTK_WIDGET (name_row));
  adw_preferences_group_add (form, GTK_WIDGET (ip_row));
  adw_preferences_group_add (form, GTK_WIDGET (policy_row));

  adw_alert_dialog_set_extra_child (alert, GTK_WIDGET (form));
  adw_alert_dialog_add_responses (alert,
                                  "cancel", _("Cancel"),
                                  "save", _("Save"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (alert, "save", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "save");

  ServerDialogData *d = g_new0 (ServerDialogData, 1);
  d->page = self;
  d->is_edit = is_edit;
  d->index = index;
  d->name_row = name_row;
  d->ip_row = ip_row;
  d->policy_row = policy_row;

  g_signal_connect (alert, "response", G_CALLBACK (on_server_dialog_response), d);
  adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
}

static void
on_edit_server_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherServersPage *self = NUVASHALAUNCHER_SERVERS_PAGE (user_data);
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "nuvashalauncher-idx"));
  present_server_dialog (self, TRUE, index);
}

typedef struct {
  NuvashaLauncherServersPage *page;
  guint index;
} DeleteServerData;

static void
on_delete_server_response (AdwAlertDialog *dialog G_GNUC_UNUSED,
                           const char *response,
                           gpointer user_data)
{
  DeleteServerData *d = user_data;
  if (g_strcmp0 (response, "delete") == 0 && d->index < d->page->servers->len)
    {
      g_ptr_array_remove_index (d->page->servers, d->index);
      persist_and_reload (d->page);
    }
  g_free (d);
}

static void
on_delete_server_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherServersPage *self = NUVASHALAUNCHER_SERVERS_PAGE (user_data);
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "nuvashalauncher-idx"));
  if (index >= self->servers->len)
    return;

  ServerEntry *s = g_ptr_array_index (self->servers, index);
  g_autofree char *body =
    g_strdup_printf (_("Remove server \"%s\"?"), s->name != NULL ? s->name : s->ip);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (_("Remove Server"), body));
  adw_alert_dialog_add_responses (alert,
                                  "cancel", _("Cancel"),
                                  "delete", _("Delete"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (alert, "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");

  DeleteServerData *d = g_new0 (DeleteServerData, 1);
  d->page = self;
  d->index = index;
  g_signal_connect (alert, "response", G_CALLBACK (on_delete_server_response), d);
  adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
}

static GtkWidget *
server_icon_widget (const char *icon_b64)
{
  GtkWidget *icon = nuvashalauncher_icon_widget_new ("network-server-symbolic");

  if (icon_b64 != NULL && *icon_b64 != '\0')
    {
      gsize raw_len = 0;
      g_autofree guchar *raw = g_base64_decode (icon_b64, &raw_len);
      if (raw != NULL && raw_len > 0)
        {
          g_autoptr (GBytes) bytes = g_bytes_new (raw, raw_len);
          g_autoptr (GdkTexture) texture = gdk_texture_new_from_bytes (bytes, NULL);
          if (texture != NULL)
            nuvashalauncher_icon_widget_set_texture (icon, texture);
        }
    }

  return icon;
}

static GtkWidget *
server_action_button (const char *icon_name, const char *tooltip, guint index,
                      GCallback cb, NuvashaLauncherServersPage *self)
{
  GtkWidget *btn = gtk_button_new_from_icon_name (icon_name);
  gtk_widget_add_css_class (btn, "flat");
  gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (btn, tooltip);
  g_object_set_data (G_OBJECT (btn), "nuvashalauncher-idx", GUINT_TO_POINTER (index));
  g_signal_connect_object (btn, "clicked", cb, self, 0);
  return btn;
}

static GtkWidget *
build_server_row (NuvashaLauncherServersPage *self, ServerEntry *s, guint index)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_subtitle_lines (row, 2);
  adw_action_row_set_subtitle_selectable (row, FALSE);
  gtk_widget_add_css_class (GTK_WIDGET (row), "nuvashalauncher-content-file-row");
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 s->name != NULL && *s->name != '\0' ? s->name : s->ip);

  adw_action_row_set_subtitle (row, s->ip != NULL ? s->ip : "");

  GtkWidget *icon = server_icon_widget (s->icon_b64);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (row, icon);

  adw_action_row_add_suffix (
    row, server_action_button ("media-playback-start-symbolic", _("Play"),
                               index, G_CALLBACK (on_join_server_clicked), self));
  GtkWidget *up = server_action_button ("go-up-symbolic", _("Move Up"),
                                        index, G_CALLBACK (on_move_up_clicked), self);
  gtk_widget_set_sensitive (up, index > 0);
  adw_action_row_add_suffix (row, up);

  GtkWidget *down = server_action_button ("go-down-symbolic", _("Move Down"),
                                          index, G_CALLBACK (on_move_down_clicked), self);
  gtk_widget_set_sensitive (down, index + 1 < self->servers->len);
  adw_action_row_add_suffix (row, down);

  adw_action_row_add_suffix (
    row, server_action_button ("document-edit-symbolic", _("Edit"),
                               index, G_CALLBACK (on_edit_server_clicked), self));
  adw_action_row_add_suffix (
    row, server_action_button ("user-trash-symbolic", _("Delete"),
                               index, G_CALLBACK (on_delete_server_clicked), self));

  return GTK_WIDGET (row);
}

static gboolean
server_matches_query (const ServerEntry *s, const char *query)
{
  if (query == NULL || *query == '\0')
    return TRUE;
  g_autofree char *q = g_utf8_casefold (query, -1);
  if (s->name != NULL)
    {
      g_autofree char *n = g_utf8_casefold (s->name, -1);
      if (strstr (n, q) != NULL)
        return TRUE;
    }
  if (s->ip != NULL)
    {
      g_autofree char *ip = g_utf8_casefold (s->ip, -1);
      if (strstr (ip, q) != NULL)
        return TRUE;
    }
  return FALSE;
}

static void
filter_server_rows (NuvashaLauncherServersPage *self)
{
  const char *query = self->search_bar != NULL
    ? gtk_editable_get_text (GTK_EDITABLE (self->search_bar))
    : "";

  for (guint i = 0; i < self->rows->len && i < self->servers->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (self->rows, i);
      ServerEntry *s = g_ptr_array_index (self->servers, i);

      gtk_widget_set_visible (row, server_matches_query (s, query));
    }
}

static void
rebuild_server_rows (NuvashaLauncherServersPage *self)
{
  for (guint i = 0; i < self->rows->len; i++)
    adw_preferences_group_remove (self->group, g_ptr_array_index (self->rows, i));
  g_ptr_array_set_size (self->rows, 0);

  for (guint i = 0; i < self->servers->len; i++)
    {
      ServerEntry *s = g_ptr_array_index (self->servers, i);
      GtkWidget *row = build_server_row (self, s, i);
      g_ptr_array_add (self->rows, row);
      adw_preferences_group_add (self->group, row);
    }

  filter_server_rows (self);
  adw_view_stack_set_visible_child_name (
    self->stack, self->servers->len > 0 ? "list" : "empty");
}

static void
on_server_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  filter_server_rows (NUVASHALAUNCHER_SERVERS_PAGE (user_data));
}

static gboolean
on_server_search_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
                      guint keyval,
                      guint keycode G_GNUC_UNUSED,
                      GdkModifierType state,
                      gpointer user_data)
{
  NuvashaLauncherServersPage *self = NUVASHALAUNCHER_SERVERS_PAGE (user_data);

  if (keyval == GDK_KEY_f && (state & GDK_CONTROL_MASK))
    {
      nuvashalauncher_servers_page_set_search_visible (self, TRUE);
      return GDK_EVENT_STOP;
    }
  if (keyval == GDK_KEY_Escape && nuvashalauncher_servers_page_is_search_visible (self))
    {
      nuvashalauncher_servers_page_set_search_visible (self, FALSE);
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

void
nuvashalauncher_servers_page_reload (NuvashaLauncherServersPage *self)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_SERVERS_PAGE (self));

  g_ptr_array_set_size (self->servers, 0);

  if (self->servers_dat_path != NULL &&
      g_file_test (self->servers_dat_path, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (NuvashaLauncherNbtTag) root =
        nuvashalauncher_nbt_parse_file (self->servers_dat_path, NULL);
      NuvashaLauncherNbtTag *list =
        root != NULL ? nuvashalauncher_nbt_compound_get (root, "servers") : NULL;

      if (list != NULL && list->type == NUVASHALAUNCHER_NBT_LIST &&
          list->v_list.items != NULL)
        {
          for (guint i = 0; i < list->v_list.items->len; i++)
            {
              NuvashaLauncherNbtTag *c = g_ptr_array_index (list->v_list.items, i);
              if (c->type != NUVASHALAUNCHER_NBT_COMPOUND)
                continue;
              ServerEntry *s = g_new0 (ServerEntry, 1);
              s->name = g_strdup (nuvashalauncher_nbt_compound_get_string (c, "name"));
              s->ip = g_strdup (nuvashalauncher_nbt_compound_get_string (c, "ip"));
              s->icon_b64 = g_strdup (nuvashalauncher_nbt_compound_get_string (c, "icon"));
              s->has_accept_textures =
                nuvashalauncher_nbt_compound_get (c, "acceptTextures") != NULL;
              s->accept_textures =
                nuvashalauncher_nbt_compound_get_byte (c, "acceptTextures", 0);
              s->has_hidden = nuvashalauncher_nbt_compound_get (c, "hidden") != NULL;
              s->hidden = nuvashalauncher_nbt_compound_get_byte (c, "hidden", 0);
              g_ptr_array_add (self->servers, s);
            }
        }
    }

  rebuild_server_rows (self);
}

static void
on_add_server_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  present_server_dialog (NUVASHALAUNCHER_SERVERS_PAGE (user_data), FALSE, 0);
}

static void
on_refresh_servers_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncher_servers_page_reload (NUVASHALAUNCHER_SERVERS_PAGE (user_data));
}

static void
nuvashalauncher_servers_page_dispose (GObject *object)
{
  NuvashaLauncherServersPage *self = NUVASHALAUNCHER_SERVERS_PAGE (object);

  if (self->header_bar != NULL)
    nuvashalauncher_servers_page_detach_header (self, self->header_bar);
  g_clear_object (&self->header_search_clamp);
  g_clear_object (&self->search_bar);
  g_clear_object (&self->add_button);
  g_clear_object (&self->refresh_button);

  g_clear_object (&self->instance);
  g_clear_object (&self->launcher);
  g_clear_pointer (&self->servers_dat_path, g_free);
  g_clear_pointer (&self->rows, g_ptr_array_unref);
  g_clear_pointer (&self->servers, g_ptr_array_unref);

  G_OBJECT_CLASS (nuvashalauncher_servers_page_parent_class)->dispose (object);
}

static void
nuvashalauncher_servers_page_class_init (NuvashaLauncherServersPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = nuvashalauncher_servers_page_dispose;
}

static void
nuvashalauncher_servers_page_init (NuvashaLauncherServersPage *self)
{
  self->rows = g_ptr_array_new ();
  self->servers = g_ptr_array_new_with_free_func ((GDestroyNotify) server_entry_free);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

NuvashaLauncherServersPage *
nuvashalauncher_servers_page_new (NuvashaLauncherInstance *instance,
                                  NuvashaLauncher *launcher)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_INSTANCE (instance), NULL);

  NuvashaLauncherServersPage *self = g_object_new (NUVASHALAUNCHER_TYPE_SERVERS_PAGE, NULL);
  g_autofree char *dir = NULL;

  self->instance = g_object_ref (instance);
  self->launcher = launcher != NULL ? g_object_ref (launcher) : NULL;
  g_object_get (instance, "directory", &dir, NULL);
  if (dir != NULL)
    self->servers_dat_path = g_build_filename (dir, "servers.dat", NULL);

  GtkBox *inner = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_widget_set_vexpand (GTK_WIDGET (inner), TRUE);

  GtkBox *bar = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  gtk_widget_set_margin_top (GTK_WIDGET (bar), 8);
  gtk_widget_set_margin_bottom (GTK_WIDGET (bar), 8);
  gtk_widget_set_margin_start (GTK_WIDGET (bar), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (bar), 12);

  GtkWidget *add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_set_valign (add_btn, GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (add_btn, _("Add Server"));
  g_signal_connect_object (add_btn, "clicked", G_CALLBACK (on_add_server_clicked), self, 0);

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);

  GtkWidget *refresh_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_valign (refresh_btn, GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (refresh_btn, _("Refresh"));
  g_signal_connect_object (refresh_btn, "clicked",
                           G_CALLBACK (on_refresh_servers_clicked), self, 0);

  gtk_box_append (bar, add_btn);
  gtk_box_append (bar, spacer);
  gtk_box_append (bar, refresh_btn);
  gtk_box_append (inner, GTK_WIDGET (bar));
  self->bar = bar;
  self->add_button = g_object_ref_sink (add_btn);
  self->refresh_button = g_object_ref_sink (refresh_btn);

  self->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);

  self->group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwClamp *clamp = ADW_CLAMP (adw_clamp_new ());
  gtk_widget_set_margin_top (GTK_WIDGET (clamp), 8);
  gtk_widget_set_margin_bottom (GTK_WIDGET (clamp), 16);
  gtk_widget_set_margin_start (GTK_WIDGET (clamp), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (clamp), 12);
  adw_clamp_set_child (clamp, GTK_WIDGET (self->group));

  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (scroller, GTK_WIDGET (clamp));

  AdwStatusPage *empty = ADW_STATUS_PAGE (adw_status_page_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (empty), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (empty), TRUE);
  adw_status_page_set_icon_name (empty, "network-server-symbolic");
  adw_status_page_set_title (empty, _("No Servers"));
  adw_status_page_set_description (empty, _("Servers added here are saved to servers.dat for this instance."));

  adw_view_stack_add_named (self->stack, GTK_WIDGET (scroller), "list");
  adw_view_stack_add_named (self->stack, GTK_WIDGET (empty), "empty");
  gtk_box_append (inner, GTK_WIDGET (self->stack));

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, GTK_WIDGET (inner));
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->toasts));

  self->search_bar = GTK_SEARCH_ENTRY (g_object_ref_sink (gtk_search_entry_new ()));
  gtk_search_entry_set_placeholder_text (self->search_bar, _("Search"));
  gtk_widget_set_hexpand (GTK_WIDGET (self->search_bar), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->search_bar), FALSE);
  g_signal_connect_object (self->search_bar, "changed",
                           G_CALLBACK (on_server_search_changed), self, 0);

  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  g_signal_connect_object (keys, "key-pressed",
                           G_CALLBACK (on_server_search_key), self, 0);

  nuvashalauncher_servers_page_reload (self);
  return self;
}

void
nuvashalauncher_servers_page_set_search_visible (NuvashaLauncherServersPage *self,
                                                 gboolean visible)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_SERVERS_PAGE (self));
  if (self->search_bar == NULL)
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->search_bar), visible);
  if (self->header_search_clamp != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp), visible);
  if (visible)
    gtk_widget_grab_focus (GTK_WIDGET (self->search_bar));
  else
    {
      gtk_editable_set_text (GTK_EDITABLE (self->search_bar), "");
      gtk_widget_grab_focus (GTK_WIDGET (self));
    }
}

gboolean
nuvashalauncher_servers_page_is_search_visible (NuvashaLauncherServersPage *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SERVERS_PAGE (self), FALSE);
  return self->search_bar != NULL &&
         gtk_widget_get_visible (GTK_WIDGET (self->search_bar));
}

void
nuvashalauncher_servers_page_attach_header (NuvashaLauncherServersPage *self,
                                            AdwHeaderBar *header)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_SERVERS_PAGE (self));
  g_return_if_fail (ADW_IS_HEADER_BAR (header));

  if (self->header_bar == header)
    return;
  if (self->header_bar != NULL)
    nuvashalauncher_servers_page_detach_header (self, self->header_bar);

  self->header_bar = header;
  if (self->header_search_clamp == NULL)
    {
      self->header_search_clamp = ADW_CLAMP (g_object_ref_sink (adw_clamp_new ()));
      adw_clamp_set_maximum_size (self->header_search_clamp, 460);
      gtk_widget_set_hexpand (GTK_WIDGET (self->header_search_clamp), TRUE);
    }

  adw_clamp_set_child (self->header_search_clamp, GTK_WIDGET (self->search_bar));
  gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp),
                          gtk_widget_get_visible (GTK_WIDGET (self->search_bar)));
  adw_header_bar_set_title_widget (header, GTK_WIDGET (self->header_search_clamp));

  if (gtk_widget_get_parent (self->add_button) == GTK_WIDGET (self->bar))
    gtk_box_remove (self->bar, self->add_button);
  adw_header_bar_pack_start (header, self->add_button);

  if (gtk_widget_get_parent (self->refresh_button) == GTK_WIDGET (self->bar))
    gtk_box_remove (self->bar, self->refresh_button);
  adw_header_bar_pack_end (header, self->refresh_button);

  gtk_widget_set_visible (GTK_WIDGET (self->bar), FALSE);
}

void
nuvashalauncher_servers_page_detach_header (NuvashaLauncherServersPage *self,
                                            AdwHeaderBar *header)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_SERVERS_PAGE (self));
  if (self->header_bar != header || header == NULL)
    return;

  self->header_bar = NULL;
  adw_header_bar_set_title_widget (header, NULL);
  if (gtk_widget_is_ancestor (self->add_button, GTK_WIDGET (header)))
    adw_header_bar_remove (header, self->add_button);
  if (gtk_widget_is_ancestor (self->refresh_button, GTK_WIDGET (header)))
    adw_header_bar_remove (header, self->refresh_button);
  if (gtk_widget_get_parent (self->add_button) != GTK_WIDGET (self->bar))
    gtk_box_prepend (self->bar, self->add_button);
  if (gtk_widget_get_parent (self->refresh_button) != GTK_WIDGET (self->bar))
    gtk_box_append (self->bar, self->refresh_button);
  gtk_widget_set_visible (GTK_WIDGET (self->bar), TRUE);
  if (self->header_search_clamp != NULL)
    adw_clamp_set_child (self->header_search_clamp, NULL);
}
