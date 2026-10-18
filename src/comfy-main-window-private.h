/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_MAIN_WINDOW_PRIVATE_H
#define comfy_MAIN_WINDOW_PRIVATE_H

#include "comfy-main-window.h"
#include "comfy-main-window-model.h"
#include "comfy-launcher.h"
#include "comfy-accounts.h"
#include "comfy-downloads.h"
#include "comfy-settings.h"
#include "comfy-users-popover.h"

struct _ComfyMainWindow
{
  AdwApplicationWindow parent_instance;

  ComfyMainWindowModel *model;

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
    GHashTable *rows;    /* ComfyDownload* -> DownloadRow* */
  } downloads;
};


void refresh_downloads (ComfyMainWindow *self);
void on_downloads_changed (ComfyDownloads *downloads, gpointer user_data);
void comfy_main_window_build_ui (ComfyMainWindow *self);
void download_row_free (gpointer data);
ComfyLauncher *window_launcher (ComfyMainWindow *self, const char *instance_id);
void update_visible_page (ComfyMainWindow *self);
GtkWidget *create_instance_row (gpointer item, gpointer user_data);
void on_row_activate (GtkListBox *list, GtkListBoxRow *row,
                      gpointer user_data);
void on_instances_changed (ComfyMainWindow *self, GListModel *model,
                           guint position, guint removed, guint added);
GMenu *create_add_menu (void);
GMenu *create_app_menu (void);
void set_menu_item_tooltip (GtkWidget *root, const char *label_text,
                            const char *tooltip);
extern const GActionEntry window_entries[];
extern const guint window_entries_count;

#endif /* comfy_MAIN_WINDOW_PRIVATE_H */
