/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_DOWNLOAD_MODPACK_DIALOG_PRIVATE_H
#define nuvashalauncher_DOWNLOAD_MODPACK_DIALOG_PRIVATE_H
#include "nuvashalauncher-ui-feedback.h"

#include "nuvashalauncher-download-modpack-dialog.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-modrinth.h"

struct _nuvashalauncherDownloadModpackDialog
{
  AdwDialog parent_instance;

  nuvashalauncherInstanceManager *manager; /* owned */

  AdwNavigationView *nav;

  /* The cache of one downloaded pack, reused while the dialog lives. */
  char *pack_path;
  gboolean imported;

  /* User selections from the details page. */
  nuvashalauncherModrinthProject *project;
  nuvashalauncherModrinthVersion *version;

  nuvashalauncherModpack *modpack; /* owned, parsed from pack_path */

  /* Browse page widgets. */
  GtkEntry *search_entry;
  GtkDropDown *filter_version_dropdown;
  GtkStringList *filter_version_model;
  nuvashalauncherTaskScope *version_catalog;
  GtkPopover *filter_popover;
  GPtrArray *filter_versions; /* selected strings */
  GPtrArray *filter_loaders;  /* selected strings */
  char *sort;
  AdwStatusPage *results_status;
  AdwViewStack *results_stack; /* one state at a time: loading, results,
                                  empty or error */
  AdwPreferencesGroup *results_group;
  GtkBox *loading_more;       /* spinner row at the bottom of the list */
  GtkBox *retry_row;          /* inline retry row when a page fails */
  GtkLabel *retry_label;
  GtkButton *more_button;     /* asks for the failed page again */
  AdwStatusPage *error_status; /* the status page of the error state */
  guint offset;
  guint64 total;
  gboolean loading_page;      /* a page is on its way */
  gboolean append;            /* the page being fetched follows the list */
  gboolean searched;          /* the first search has been asked for */
  guint search_generation;   /* rejects answers from older searches */
  GHashTable *seen_ids;       /* project id -> NULL, no duplicate rows */

  /* Split view: browser on the left, selected pack on the right. */
  AdwNavigationSplitView *split;
  AdwNavigationPage *details_page; /* weak: the page of the open pack */
  GtkDropDown *dropdown;       /* the versions to install */
  GtkButton *download_button;
  GtkBox *body;                /* the box the Markdown description goes in */
  GtkLabel *meta;              /* license and author under the title */
  GtkLabel *count;             /* the download count, at the left */

  /* State for the pack details dialog. */
  GPtrArray *versions; /* nuvashalauncherModrinthVersion* of the details dialog */
  AdwEntryRow *name_entry;
  AdwActionRow *directory_row;
  GtkLabel *details_error; /* inline words when versions fail or are empty */

  /* Whether the description and versions have loaded. */
  gboolean details_ready;
  gboolean versions_ready;
  guint details_generation;  /* rejects answers from older projects */
  nuvashalauncherModrinthDetails *details; /* owned, the answer read while the dialog is up */
  char *details_problem;       /* why the description is not there */
  char *versions_problem;      /* why the versions are not there */

  /* No install page is needed; installation appears in the header and the
   * dialog closes when it starts. */

  /* Pack download shown in the header. Weak: the
     manager owns it, and the pointer goes NULL when it is let go. It is
     the same entry for both phases, the compressed pack and the extract. */
  nuvashalauncherDownload *pack_download;

  /* Operation being retried after an error. */
  gboolean fail_is_install;

  /* Set when closed. Installation continues, but pack downloads are
   * cancelled and callbacks must not access widgets. */
  gboolean closed;

  /* Optional-file switches and their corresponding files. Reset on each
   * visit. */
  GPtrArray *opt_switch_rows; /* AdwSwitchRow*/
  GPtrArray *opt_files;       /* borrowed/self counterpart list */


  /* The running parts. */
  GCancellable *search; /* kills an in-flight search */
  GCancellable *fetch;  /* kills a version fetch, the pack get and install */
  guint debounce;
  guint import_idle;
  nuvashalauncherTaskScope *scope;
  GPtrArray *rows;
  GPtrArray *hits;

  char *directory;     /* where the instance goes, NULL for the default */
  char *instance_name; /* name entered on the details page */
  char *icon_path;     /* the fetched icon of the pack, NULL when it has
                          none; the new instance is given it */
};


void operation_done (gpointer user_data);
void details_watch (gpointer field, GtkWidget *widget);
void build_browse_page (nuvashalauncherDownloadModpackDialog *, gboolean start_search);
void close_details (nuvashalauncherDownloadModpackDialog *);
GtkWidget *details_content (nuvashalauncherDownloadModpackDialog *);
void dialog_details_open (nuvashalauncherDownloadModpackDialog *);
void on_modpack_download_clicked (GtkButton *, gpointer);
GtkWidget *build_error_page (nuvashalauncherDownloadModpackDialog *, const char *,
                             const char *, GCallback, GCallback);
void push_failure (nuvashalauncherDownloadModpackDialog *, const char *, const GError *, GCallback);
void push_error (nuvashalauncherDownloadModpackDialog *, const char *, const char *,
                 GCallback);
void dialog_details_show_again (nuvashalauncherDownloadModpackDialog *);
void on_pack_parsed (GObject *, GAsyncResult *, gpointer);
gboolean dialog_start_features (nuvashalauncherDownloadModpackDialog *);
void start_install (nuvashalauncherDownloadModpackDialog *);
void remove_pack_file (nuvashalauncherDownloadModpackDialog *);
void on_error_back (GtkButton *, gpointer);
void on_error_retry_fetch (GtkButton *, gpointer);
void on_error_retry_install (GtkButton *, gpointer);

#endif
