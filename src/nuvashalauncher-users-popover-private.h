#ifndef NUVASHA_USERS_POPOVER_PRIVATE_H
#define NUVASHA_USERS_POPOVER_PRIVATE_H

#include "nuvashalauncher-users-popover.h"
#include "nuvashalauncher-skin-view.h"
#include <libsoup/soup.h>

struct _nuvashalauncherUsersPopover
{
  GtkPopover parent_instance;
  nuvashalauncherAccounts *accounts;
  GtkListBox *list_box;
  GtkScrolledWindow *list_scroller;
  GtkLabel *error_label;
  GtkImage *avatar_image;
  GtkBox *button_content;
  GCancellable *avatars;
  GHashTable *avatar_cache;
  GHashTable *profile_cache; /* account UUID -> last fetched profile JSON */
  char *active_name;
};

void nuvashalauncher_users_set_avatar (nuvashalauncherUsersPopover *, GtkImage *, const char *);
GdkTexture *face_texture (GdkTexture *skin);
void nuvashalauncher_users_set_avatar_icon (GtkImage *);
gboolean nuvashalauncher_users_set_local_avatar (GtkImage *, const char *);
char *nuvashalauncher_users_default_steve_path (void);
AdwDialog *nuvashalauncher_users_skin_menu_new (nuvashalauncherUsersPopover *, GtkImage *, nuvashalauncherAccount *);

typedef struct _SkinMenu SkinMenu;
SkinMenu *skin_menu_ref (SkinMenu *);
void skin_menu_unref (SkinMenu *);
void on_skin_dialog_close_attempt (AdwDialog *, gpointer);
void on_skin_dialog_closed (AdwDialog *, gpointer);
void on_model_toggled (GtkCheckButton *, gpointer);
void load_profile_skins (SkinMenu *);
void load_view_texture (nuvashalauncherSkinView *, const char *, gboolean);
void upload_selected_skin (SkinMenu *, const char *, const char *);
void on_cape_selected (GObject *, GParamSpec *, gpointer);
void on_active_skin_delete_done (GObject *, GAsyncResult *, gpointer);
void delete_active_skin (SkinMenu *);
void append_profile_skins (SkinMenu *, const char *);
void select_saved_skin (SkinMenu *);
void apply_profile_json (SkinMenu *, const char *);
void on_capes_loaded (GObject *, GAsyncResult *, gpointer);

struct _SkinMenu
{
  grefcount refs;
  nuvashalauncherUsersPopover *owner;
  GtkImage *image;
  nuvashalauncherSkinView *view;
  AdwDialog *dialog;
  GtkListBox *skins;
  GtkDropDown *capes;
  GtkStringList *cape_list;
  GPtrArray *cape_ids;
  GPtrArray *cape_urls;
  GHashTable *skin_urls;
  AdwActionRow *current_skin_row;
  GtkButton *remove_button;
  GtkCheckButton *default_model;
  GtkCheckButton *slim_model;
  gboolean applying_profile;
  gboolean closed;
  gboolean skin_upload_in_flight;
  gint64 skin_upload_retry_after;
  guint skin_upload_retry_source;
  guint skin_upload_retry_attempt;
  char *skin_upload_pending_url;
  char *skin_upload_pending_variant;
  GCancellable *skin_upload_cancellable;
  char *pending_cape_id;
  GCancellable *cancellable;
  gboolean defaults_added;
  GHashTable *deleted_skin_ids;
  nuvashalauncherAccount *account;
};

AdwActionRow *skin_row_new (SkinMenu *, const char *, const char *);
char *skin_directory_path (void);
char *skin_name_for_url (const char *);
char *skin_display_name (const char *);
char *unique_skin_filename (const char *, const char *);
char *cached_skin_path (const char *);
char *cached_cape_path (const char *);
gboolean is_default_skin_alias (const char *);
gboolean skin_url_is_default (const char *);
void append_local_skins (SkinMenu *);
AdwActionRow *append_default_skins (SkinMenu *);

#endif
