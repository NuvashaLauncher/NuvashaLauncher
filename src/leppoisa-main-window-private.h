/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_MAIN_WINDOW_PRIVATE_H
#define leppoisa_MAIN_WINDOW_PRIVATE_H

#include "leppoisa-main-window.h"
#include "leppoisa-main-window-model.h"
#include "leppoisa-launcher.h"
#include "leppoisa-accounts.h"
#include "leppoisa-downloads.h"
#include "leppoisa-settings.h"
#include "leppoisa-users-popover.h"

struct _LeppoisaMainWindow
{
  AdwApplicationWindow parent_instance;

  LeppoisaMainWindowModel *model;

  /* Core window UI state. */
  struct {
    GtkStack *stack;    /* empty state or instance list */
    AdwBanner *banner;  /* launch failures and crash details */
  } ui;

  /* What is downloading right now, shown next to the "+" menu. */
  struct {
    GtkMenuButton *button; /* around the spinner */
    AdwSpinner *spinner;
    GtkImage *icon;        /* what the button opens beside the spinner */
    GtkBox *list;          /* the box inside the dropdown */
    GHashTable *rows;    /* LeppoisaDownload* -> DownloadRow* */
  } downloads;
};


void refresh_downloads (LeppoisaMainWindow *self);
void on_downloads_changed (LeppoisaDownloads *downloads, gpointer user_data);
void leppoisa_main_window_build_ui (LeppoisaMainWindow *self);
void download_row_free (gpointer data);
LeppoisaLauncher *window_launcher (LeppoisaMainWindow *self, const char *instance_id);
void update_visible_page (LeppoisaMainWindow *self);
GtkWidget *create_instance_row (gpointer item, gpointer user_data);
void on_row_activate (GtkListBox *list, GtkListBoxRow *row,
                      gpointer user_data);
void on_instances_changed (LeppoisaMainWindow *self, GListModel *model,
                           guint position, guint removed, guint added);
GMenu *create_add_menu (void);
GMenu *create_app_menu (void);
void set_menu_item_tooltip (GtkWidget *root, const char *label_text,
                            const char *tooltip);
extern const GActionEntry window_entries[];
extern const guint window_entries_count;

#endif /* leppoisa_MAIN_WINDOW_PRIVATE_H */
