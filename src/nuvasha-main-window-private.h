/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_MAIN_WINDOW_PRIVATE_H
#define nuvasha_MAIN_WINDOW_PRIVATE_H

#include "nuvasha-main-window.h"
#include "nuvasha-main-window-model.h"
#include "nuvasha-launcher.h"
#include "nuvasha-accounts.h"
#include "nuvasha-downloads.h"
#include "nuvasha-settings.h"
#include "nuvasha-users-popover.h"

struct _NuvashaMainWindow
{
  AdwApplicationWindow parent_instance;

  NuvashaMainWindowModel *model;

  /* Core window UI state. */
  struct {
    AdwViewStack *stack; /* empty state or instance list */
    AdwBanner *banner;  /* launch failures and crash details */
  } ui;

  /* Active downloads shown next to the "+" menu. */
  struct {
    GtkMenuButton *button; /* around the spinner */
    AdwSpinner *spinner;
    GtkImage *icon;        /* icon beside the spinner */
    GtkBox *list;          /* the box inside the dropdown */
    GHashTable *rows;    /* NuvashaDownload* -> DownloadRow* */
  } downloads;
};


void refresh_downloads (NuvashaMainWindow *self);
void on_downloads_changed (NuvashaDownloads *downloads, gpointer user_data);
void nuvasha_main_window_build_ui (NuvashaMainWindow *self);
void download_row_free (gpointer data);
NuvashaLauncher *window_launcher (NuvashaMainWindow *self, const char *instance_id);
void update_visible_page (NuvashaMainWindow *self);
GtkWidget *create_instance_row (gpointer item, gpointer user_data);
void on_row_activate (GtkListBox *list, GtkListBoxRow *row,
                      gpointer user_data);
void on_instances_changed (NuvashaMainWindow *self, GListModel *model,
                           guint position, guint removed, guint added);
GMenu *create_add_menu (void);
GMenu *create_app_menu (void);
void set_menu_item_tooltip (GtkWidget *root, const char *label_text,
                            const char *tooltip);
extern const GActionEntry window_entries[];
extern const guint window_entries_count;

#endif /* nuvasha_MAIN_WINDOW_PRIVATE_H */
