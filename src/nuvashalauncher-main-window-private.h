/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_MAIN_WINDOW_PRIVATE_H
#define nuvashalauncher_MAIN_WINDOW_PRIVATE_H

#include "nuvashalauncher-main-window.h"
#include "nuvashalauncher-main-window-model.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-users-popover.h"

struct _nuvashalauncherMainWindow
{
  AdwApplicationWindow parent_instance;

  nuvashalauncherMainWindowModel *model;
  GSettings *window_settings;
  guint background_quit_source;

  /* Core window UI state. */
  struct {
    AdwViewStack *stack; /* empty state or instance list */
    AdwBanner *banner;  /* launch failures and crash details */
    AdwToolbarView *toolbar;
    GtkListBox *instances;
    GtkButton *edit;
    GtkButton *footer_delete;
    GtkButton *footer_export;
    GtkActionBar *selection_footer;
    GtkLabel *selection_label;
    GHashTable *selected_instances; /* instance ID -> TRUE */
    gboolean edit_mode;
  } ui;

  /* Active downloads shown next to the "+" menu. */
  struct {
    GtkMenuButton *button; /* around the spinner */
    AdwSpinner *spinner;
    GtkImage *icon;        /* icon beside the spinner */
    GtkBox *list;          /* the box inside the dropdown */
    GHashTable *rows;    /* nuvashalauncherDownload* -> DownloadRow* */
  } downloads;
};


void refresh_downloads (nuvashalauncherMainWindow *self);
void on_downloads_changed (nuvashalauncherDownloads *downloads, gpointer user_data);
void nuvashalauncher_main_window_build_ui (nuvashalauncherMainWindow *self);
void download_row_free (gpointer data);
nuvashalauncher *window_launcher (nuvashalauncherMainWindow *self, const char *instance_id);
void update_visible_page (nuvashalauncherMainWindow *self);
void nuvashalauncher_main_window_show_crash_logs (nuvashalauncherMainWindow *self,
                                          nuvashalauncher *launcher);
void nuvashalauncher_main_window_maybe_quit_background (nuvashalauncherMainWindow *self);
GtkWidget *create_instance_row (gpointer item, gpointer user_data);
void on_row_activate (GtkListBox *list, GtkListBoxRow *row,
                      gpointer user_data);
void on_instances_changed (nuvashalauncherMainWindow *self, GListModel *model,
                           guint position, guint removed, guint added);
void set_instance_edit_mode (nuvashalauncherMainWindow *self);
void update_instance_selection_ui (nuvashalauncherMainWindow *self);
void bulk_export_instances (nuvashalauncherMainWindow *self);
void bulk_duplicate_instances (nuvashalauncherMainWindow *self);
void bulk_delete_instances (nuvashalauncherMainWindow *self);
GMenu *create_add_menu (void);
GMenu *create_app_menu (void);
void set_menu_item_tooltip (GtkWidget *root, const char *label_text,
                            const char *tooltip);
extern const GActionEntry window_entries[];
extern const guint window_entries_count;

#endif /* nuvashalauncher_MAIN_WINDOW_PRIVATE_H */
