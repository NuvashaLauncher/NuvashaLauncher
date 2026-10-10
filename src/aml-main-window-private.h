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

  AmlInstanceManager *manager;
  AmlSettings *settings;

  /* The accounts a launch plays as, and the popover in the header that
     shows them and the two ways of adding one. The window owns the store
     for as long as it is open, and every launcher of an instance is
     handed the same one. */
  AmlAccounts *accounts;
  AmlUsersPopover *users_popover;

  /* One launcher per instance, keyed by its id, so the row of an
     instance and its dialog always talk about the same launch. */
  GHashTable *launchers;

  /* The dialog of each instance that is open, keyed by its id. An
     AdwDialog is not a window, so the window list of the application
     cannot be asked about it; this is the bookkeeping that is. The
     dialogs are held by the window they were presented on, so the table
     holds borrowed pointers and only says which ones are open. */
  GHashTable *instance_dialogs;

  /* Switches between the empty state and the instance list. The list is
     what the window shows; an instance opens a dialog over it. */
  GtkWidget *stack;

  /* What a launch ended with, which is worth reading after it happened:
     a toast is gone in seconds, a banner stays until it is dismissed. */
  GtkWidget *banner;

  /* What is downloading right now, shown next to the "+" menu: a spinner
     while anything is on its way, and a dropdown listing each entry. The
     rows are keyed by the AmlDownload they show. */
  GtkWidget *downloads_button;  /* a GtkMenuButton around the spinner */
  GtkWidget *downloads_spinner;
  GtkWidget *downloads_icon;      /* what the button opens, beside the spinner */
  GtkWidget *downloads_list;    /* the box inside the dropdown */
  GHashTable *download_rows;    /* AmlDownload* -> DownloadRow* */
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
