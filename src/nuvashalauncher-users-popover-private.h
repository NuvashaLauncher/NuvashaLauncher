/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_USERS_POPOVER_PRIVATE_H
#define NUVASHALAUNCHER_USERS_POPOVER_PRIVATE_H

#include "nuvashalauncher-users-popover.h"
#include "nuvashalauncher-skin-view.h"
#include <libsoup/soup.h>

struct _NuvashaLauncherUsersPopover
{
  GtkPopover parent_instance;
  NuvashaLauncherAccounts *accounts;
  GtkListBox *list_box;
  GtkScrolledWindow *list_scroller;
  GtkButton *offline_button;
  GtkLabel *error_label;
  GtkImage *avatar_image;
  GtkBox *button_content;
  GCancellable *avatars;
  GHashTable *avatar_cache;
  GHashTable *profile_cache; /* account UUID -> last fetched profile JSON */
  char *active_name;
};

void nuvashalauncher_users_set_avatar (NuvashaLauncherUsersPopover *, GtkImage *, const char *);
GdkTexture *face_texture (GdkTexture *skin);
void nuvashalauncher_users_set_avatar_icon (GtkImage *);
gboolean nuvashalauncher_users_set_local_avatar (GtkImage *, const char *);
/* TRUE when NUVASHALAUNCHER_TEST_NO_NETWORK is set, so tests never reach
 * the network for avatars and skin previews. */
gboolean nuvashalauncher_users_network_disabled (void);
/* Shows the head of @account in @image: instantly from the skin on disk
 * when it is there, and fetched once (then kept on disk) when it is not.
 * The default icon stays only when the account has no skin at all. */
void nuvashalauncher_users_show_account_head (NuvashaLauncherUsersPopover *owner,
                                              GtkImage                    *image,
                                              const NuvashaLauncherAccount *account);
AdwDialog *nuvashalauncher_users_skin_menu_new (NuvashaLauncherUsersPopover *, GtkImage *, NuvashaLauncherAccount *);

void load_view_texture (NuvashaLauncherSkinView *, const char *, gboolean);

typedef AdwActionRow *(*NuvashaLauncherSkinRowFactory) (const char *title,
                                                        const char *url,
                                                        gpointer    user_data);

char *skin_directory_path (void);
char *skin_name_for_url (const char *);
char *skin_display_name (const char *);
char *unique_skin_filename (const char *, const char *);
char *cached_skin_path (const char *);
char *cached_cape_path (const char *);
gboolean is_default_skin_alias (const char *);
void append_local_skins (GtkListBox                   *skins,
                         GHashTable                   *skin_urls,
                         GHashTable                   *deleted_skin_ids,
                         NuvashaLauncherSkinRowFactory row_factory,
                         gpointer                      user_data);

#endif
