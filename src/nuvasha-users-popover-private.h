#ifndef NUVASHA_USERS_POPOVER_PRIVATE_H
#define NUVASHA_USERS_POPOVER_PRIVATE_H

#include "nuvasha-users-popover.h"
#include "nuvasha-skin-view.h"
#include <libsoup/soup.h>

struct _NuvashaUsersPopover
{
  GtkPopover parent_instance;
  NuvashaAccounts *accounts;
  GtkListBox *list_box;
  GtkScrolledWindow *list_scroller;
  GtkLabel *error_label;
  GtkImage *avatar_image;
  GtkBox *button_content;
  GCancellable *avatars;
  GHashTable *avatar_cache;
  char *active_name;
};

void nuvasha_users_set_avatar (NuvashaUsersPopover *, GtkImage *, const char *);
GdkTexture *face_texture (GdkTexture *skin);
void nuvasha_users_set_avatar_icon (GtkImage *);
gboolean nuvasha_users_set_local_avatar (GtkImage *, const char *);
char *nuvasha_users_default_steve_path (void);
AdwDialog *nuvasha_users_skin_menu_new (NuvashaUsersPopover *, GtkImage *, NuvashaAccount *);

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
  GtkCheckButton *default_model;
  GtkCheckButton *slim_model;
  gboolean applying_profile;
  gboolean skin_upload_in_flight;
  gboolean defaults_added;
  SoupMessage *profile_message;
  SoupMessage *cape_message;
  GHashTable *deleted_skin_ids;
  NuvashaAccount *account;
} SkinMenu;

AdwActionRow *skin_row_new (SkinMenu *, const char *, const char *);
char *skin_directory_path (void);
char *skin_name_for_url (const char *);
char *skin_display_name (const char *);
char *unique_skin_filename (const char *, const char *);
char *cached_skin_path (const char *);
gboolean is_default_skin_alias (const char *);
gboolean skin_url_is_default (const char *);
void append_local_skins (SkinMenu *);
AdwActionRow *append_default_skins (SkinMenu *);

#endif
