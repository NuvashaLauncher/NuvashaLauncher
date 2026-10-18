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
  NuvashaLogsPage *logs_page; /* the last page, kept so a rebuild can move it back */

  /* The Directory row: the chooser button that says where the instance is
     and the spinner that shows a move in progress. A move in progress
     makes the button insensitive, so a second one cannot start on top of
     it. */
  AdwActionRow *directory_row;
  GtkButton *directory_change;
  GtkLabel *directory_path_label; /* the path inside the chooser button */
  AdwSpinner *directory_spinner;

  /* The Java row plus what we know about it. Row load = detection
   * finishes; description load = the Minecraft version's javaVersion.  */
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

  /* The two loader rows, plain AdwComboRows added to the Minecraft group,
     and what the instance runs now, so that a change that cannot be
     written goes back to it. */
  NuvashaLoaderRow *loader_row;
  NuvashaLoader loader;
  char *loader_version;
};


char *instance_string (NuvashaInstance *instance, const char *property);
void instance_page_show_toast (NuvashaInstancePage *self, const char *message);
void update_directory_row (NuvashaInstancePage *self);
void on_change_directory_clicked (GtkButton *, gpointer);
void on_duplicate_clicked (GtkButton *, gpointer);
void on_open_folder_clicked (GtkButton *, gpointer);
void on_export_clicked (GtkButton *, gpointer);
void on_delete_clicked (GtkButton *, gpointer);

#endif
