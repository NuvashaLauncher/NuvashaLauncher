/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_INSTANCE_PAGE_PRIVATE_H
#define nuvasha_INSTANCE_PAGE_PRIVATE_H

#include "nuvasha-instance-page.h"
#include "nuvasha-java-row.h"
#include "nuvasha-loader-row.h"
#include "nuvasha-logs-page.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"

struct _NuvashaInstancePage
{
  GtkBox parent_instance;

  NuvashaInstanceManager *manager;
  NuvashaInstance *instance;
  NuvashaSettings *settings;
  NuvashaLauncher *launcher;
  gboolean export_include_transient;
  NuvashaTaskScope *scope;
  guint export_idle;

  /* Borrowed from the widget tree of the page. Cleared in dispose(),
   * because the tree can go away while a running operation still keeps
   * this page alive. */
  AdwToastOverlay *toast_overlay;
  AdwActionRow *icon_row;
  GtkLabel *icon_path_label; /* the icon path inside the chooser button */
  AdwViewStack *stack; /* the view stack holding the pages of the instance */
  AdwViewSwitcher *switcher;
  NuvashaLogsPage *logs_page; /* the last page, kept so a rebuild can move it back */

  /* Directory chooser and move-progress spinner. */
  AdwActionRow *directory_row;
  GtkButton *directory_change;
  GtkLabel *directory_path_label; /* the path inside the chooser button */
  AdwSpinner *directory_spinner;

  /* Java picker and required major version. */
  NuvashaJavaRow *java_row;
  int required_java;        /* 0 means "never found one / unknown" */

  /* The manifest and version-document reads the Java warning is built
     from. One at a time: a new version picked stops the last read. */
  NuvashaTaskScope *versions_operation;
  guint versions_generation;
  NuvashaDownload *export_download;
  NuvashaTaskScope *export_operation;
  char *export_name;
  char *export_summary;
  char *export_version;

  /* Loader controls and current persisted values for rollback. */
  NuvashaLoaderRow *loader_row;
  NuvashaLoader loader;
  char *loader_version;
};


char *instance_string (NuvashaInstance *instance, const char *property);
char *loader_display_name (NuvashaInstance *instance);
void refresh_in_list (NuvashaInstancePage *self);
void rebuild_content_pages (NuvashaInstancePage *self);
GtkWidget *create_settings_page (NuvashaInstancePage *self);
void instance_page_show_toast (NuvashaInstancePage *self, const char *message);
void update_directory_row (NuvashaInstancePage *self);
void on_change_directory_clicked (GtkButton *, gpointer);
void on_duplicate_clicked (GtkButton *, gpointer);
void on_open_folder_clicked (GtkButton *, gpointer);
void on_export_clicked (GtkButton *, gpointer);
void on_delete_clicked (GtkButton *, gpointer);

#endif
