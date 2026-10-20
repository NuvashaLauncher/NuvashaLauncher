#ifndef NUVASHA_USERS_POPOVER_PRIVATE_H
#define NUVASHA_USERS_POPOVER_PRIVATE_H

#include "nuvasha-users-popover.h"
#include "nuvasha-skin-view.h"

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

#endif
