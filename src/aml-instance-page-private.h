/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_INSTANCE_PAGE_PRIVATE_H
#define AML_INSTANCE_PAGE_PRIVATE_H

#include "aml-instance-page.h"
#include "aml-java-row.h"
#include "aml-loader-row.h"
#include "aml-logs-page.h"
#include "aml-download.h"
#include "aml-downloads.h"

struct _AmlInstancePage
{
  GtkBox parent_instance;

  AmlInstanceManager *manager;
  AmlInstance *instance;
  AmlSettings *settings;
  AmlLauncher *launcher;
  gboolean export_include_transient;
  AmlTaskScope *scope;
  guint export_idle;

  /* Borrowed from the widget tree of the page. Cleared in dispose(),
   * because the tree can go away while a running operation still keeps
   * this page alive. */
  AdwToastOverlay *toast_overlay;
  AdwActionRow *icon_row;
  GtkLabel *icon_path_label; /* the icon path inside the chooser button */
  AdwViewStack *stack; /* the view stack holding the pages of the instance */
  AmlLogsPage *logs_page; /* the last page, kept so a rebuild can move it back */

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
  AmlJavaRow *java_row;
  int required_java;        /* 0 means "never found one / unknown" */

  /* The manifest and version-document reads the Java warning is built
     from. One at a time: a new version picked stops the last read. */
  AmlTaskScope *versions_operation;
  guint versions_generation;
  AmlDownload *export_download;
  AmlTaskScope *export_operation;
  char *export_name;
  char *export_summary;
  char *export_version;

  /* The two loader rows, plain AdwComboRows added to the Minecraft group,
     and what the instance runs now, so that a change that cannot be
     written goes back to it. */
  AmlLoaderRow *loader_row;
  AmlLoader loader;
  char *loader_version;
};


char *instance_string (AmlInstance *instance, const char *property);
void instance_page_show_toast (AmlInstancePage *self, const char *message);
void update_directory_row (AmlInstancePage *self);
void on_change_directory_clicked (GtkButton *, gpointer);
void on_duplicate_clicked (GtkButton *, gpointer);
void on_open_folder_clicked (GtkButton *, gpointer);
void on_export_clicked (GtkButton *, gpointer);
void on_delete_clicked (GtkButton *, gpointer);

#endif
