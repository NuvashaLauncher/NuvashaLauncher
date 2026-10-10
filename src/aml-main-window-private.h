/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_MAIN_WINDOW_PRIVATE_H
#define AML_MAIN_WINDOW_PRIVATE_H

#include "aml-main-window.h"
#include "aml-launcher.h"
#include "aml-accounts.h"
#include "aml-downloads.h"
#include "aml-settings.h"
#include "aml-users-popover.h"

struct _AmlMainWindow
{
  AdwApplicationWindow parent_instance;

  /* Shared services used by the window, its dialogs, and its launchers. */
  struct {
    AmlInstanceManager *manager;
    AmlSettings *settings;
    AmlAccounts *accounts;
    AmlUsersPopover *users_popover;
  } services;

  /* Per-instance launchers and dialogs. Both maps are keyed by instance
   * id so a row, launcher, and dialog always refer to the same instance. */
  struct {
    GHashTable *launchers;        /* id -> owned AmlLauncher */
    GHashTable *instance_dialogs; /* id -> borrowed AdwDialog */
  } instances;

  /* Core window UI state. */
  struct {
    GtkStack *stack;    /* empty state or instance list */
    AdwBanner *banner;  /* launch failures and crash details */
  } ui;

  /* What is downloading right now, shown next to the "+" menu. */
  struct {
    GtkWidget *button;  /* a GtkMenuButton around the spinner */
    GtkWidget *spinner;
    GtkWidget *icon;    /* what the button opens beside the spinner */
    GtkWidget *list;    /* the box inside the dropdown */
    GHashTable *rows;    /* AmlDownload* -> DownloadRow* */
  } downloads;
};


void refresh_downloads (AmlMainWindow *self);
void on_downloads_changed (AmlDownloads *downloads, gpointer user_data);
void aml_main_window_build_ui (AmlMainWindow *self);
void download_row_free (gpointer data);
AmlLauncher *window_launcher (AmlMainWindow *self, const char *instance_id);
void update_visible_page (AmlMainWindow *self);
GtkWidget *create_instance_row (gpointer item, gpointer user_data);
void on_row_activate (GtkListBox *list, GtkListBoxRow *row,
                      gpointer user_data);
void on_instances_changed (AmlMainWindow *self, GListModel *model,
                           guint position, guint removed, guint added);
GMenu *create_add_menu (void);
GMenu *create_app_menu (void);
void set_menu_item_tooltip (GtkWidget *root, const char *label_text,
                            const char *tooltip);
extern const GActionEntry window_entries[];
extern const guint window_entries_count;

#endif /* AML_MAIN_WINDOW_PRIVATE_H */
