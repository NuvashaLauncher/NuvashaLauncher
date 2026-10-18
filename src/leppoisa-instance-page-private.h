/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_INSTANCE_PAGE_PRIVATE_H
#define leppoisa_INSTANCE_PAGE_PRIVATE_H

#include "leppoisa-instance-page.h"
#include "leppoisa-java-row.h"
#include "leppoisa-loader-row.h"
#include "leppoisa-logs-page.h"
#include "leppoisa-download.h"
#include "leppoisa-downloads.h"

struct _LeppoisaInstancePage
{
  GtkBox parent_instance;

  LeppoisaInstanceManager *manager;
  LeppoisaInstance *instance;
  LeppoisaSettings *settings;
  LeppoisaLauncher *launcher;
  gboolean export_include_transient;
  LeppoisaTaskScope *scope;
  guint export_idle;

  /* Borrowed from the widget tree of the page. Cleared in dispose(),
   * because the tree can go away while a running operation still keeps
   * this page alive. */
  AdwToastOverlay *toast_overlay;
  AdwActionRow *icon_row;
  GtkLabel *icon_path_label; /* the icon path inside the chooser button */
  AdwViewStack *stack; /* the view stack holding the pages of the instance */
  LeppoisaLogsPage *logs_page; /* the last page, kept so a rebuild can move it back */

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
  LeppoisaJavaRow *java_row;
  int required_java;        /* 0 means "never found one / unknown" */

  /* The manifest and version-document reads the Java warning is built
     from. One at a time: a new version picked stops the last read. */
  LeppoisaTaskScope *versions_operation;
  guint versions_generation;
  LeppoisaDownload *export_download;
  LeppoisaTaskScope *export_operation;
  char *export_name;
  char *export_summary;
  char *export_version;

  /* The two loader rows, plain AdwComboRows added to the Minecraft group,
     and what the instance runs now, so that a change that cannot be
     written goes back to it. */
  LeppoisaLoaderRow *loader_row;
  LeppoisaLoader loader;
  char *loader_version;
};


char *instance_string (LeppoisaInstance *instance, const char *property);
void instance_page_show_toast (LeppoisaInstancePage *self, const char *message);
void update_directory_row (LeppoisaInstancePage *self);
void on_change_directory_clicked (GtkButton *, gpointer);
void on_duplicate_clicked (GtkButton *, gpointer);
void on_open_folder_clicked (GtkButton *, gpointer);
void on_export_clicked (GtkButton *, gpointer);
void on_delete_clicked (GtkButton *, gpointer);

#endif
