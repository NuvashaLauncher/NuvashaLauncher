/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_INSTANCE_PAGE_PRIVATE_H
#define nuvashalauncher_INSTANCE_PAGE_PRIVATE_H

#include "nuvashalauncher-instance-page.h"
#include "nuvashalauncher-java-row.h"
#include "nuvashalauncher-loader-row.h"
#include "nuvashalauncher-logs-page.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-ui-feedback.h"

void instance_page_show_error (NuvashaLauncherInstancePage *self, const GError *error);

struct _NuvashaLauncherInstancePage
{
  GtkBox parent_instance;

  NuvashaLauncherInstanceManager *manager;
  NuvashaLauncherInstance *instance;
  NuvashaLauncherSettings *settings;
  NuvashaLauncher *launcher;
  gboolean export_include_transient;
  NuvashaLauncherTaskScope *scope;
  guint export_idle;

  /* Borrowed from the widget tree of the page. Cleared in dispose(),
   * because the tree can go away while a running operation still keeps
   * this page alive. */
  AdwToastOverlay *toast_overlay;
  AdwActionRow *icon_row;
  GtkLabel *icon_path_label; /* the icon path inside the chooser button */
  AdwViewStack *stack; /* the view stack holding the pages of the instance */
  AdwViewSwitcher *switcher;
  NuvashaLauncherLogsPage *logs_page; /* the last page, kept so a rebuild can move it back */

  /* Directory chooser and move-progress spinner. */
  AdwActionRow *directory_row;
  GtkButton *directory_change;
  GtkLabel *directory_path_label; /* the path inside the chooser button */
  AdwSpinner *directory_spinner;

  /* Java picker and required major version. */
  NuvashaLauncherJavaRow *java_row;
  int required_java;

  /* The manifest and version-document reads the Java warning is built
     from. One at a time: a new version picked stops the last read. */
  NuvashaLauncherTaskScope *versions_operation;
  guint versions_generation;
  NuvashaLauncherDownload *export_download;
  NuvashaLauncherTaskScope *export_operation;
  char *export_name;
  char *export_summary;
  char *export_version;

  /* Loader controls and current persisted values for rollback. */
  NuvashaLauncherLoaderRow *loader_row;
  NuvashaLauncherLoader loader;
  char *loader_version;
};

char *instance_string (NuvashaLauncherInstance *instance, const char *property);
char *loader_display_name (NuvashaLauncherInstance *instance);
GtkWidget *instance_page_dialog_parent (NuvashaLauncherInstancePage *self);
void refresh_in_list (NuvashaLauncherInstancePage *self);
void rebuild_content_pages (NuvashaLauncherInstancePage *self);
GtkWidget *create_settings_page (NuvashaLauncherInstancePage *self);
void instance_page_show_toast (NuvashaLauncherInstancePage *self, const char *message);
void update_directory_row (NuvashaLauncherInstancePage *self);
void on_change_directory_clicked (GtkButton *, gpointer);
void on_duplicate_clicked (GtkButton *, gpointer);
void on_open_folder_clicked (GtkButton *, gpointer);
void on_export_clicked (GtkButton *, gpointer);
void on_delete_clicked (GtkButton *, gpointer);

#endif
