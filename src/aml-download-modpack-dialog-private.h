/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_DOWNLOAD_MODPACK_DIALOG_PRIVATE_H
#define AML_DOWNLOAD_MODPACK_DIALOG_PRIVATE_H

#include "aml-download-modpack-dialog.h"
#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-modpack.h"
#include "aml-modrinth.h"

struct _AmlDownloadModpackDialog
{
  AdwDialog parent_instance;

  AmlInstanceManager *manager; /* owned */

  AdwNavigationView *nav;

  /* The cache of one downloaded pack, reused while the dialog lives. */
  char *pack_path;
  gboolean imported;

  /* What the user did by the end of the details page. */
  AmlModrinthProject *project;
  AmlModrinthVersion *version;

  AmlModpack *modpack; /* owned, parsed from pack_path */

  /* Browse page widgets. */
  GtkSearchEntry *search_entry;
  AdwStatusPage *results_status;
  GtkStack *results_stack;   /* one state at a time: loading, results,
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

  /* The browser is the left half; the pack that is opened is the right
     half, empty until one is opened. */
  AdwNavigationSplitView *split;
  AdwNavigationPage *details_page; /* weak: the page of the open pack */
  GtkDropDown *dropdown;       /* the versions to install */
  GtkButton *download_button;
  GtkBox *body;                /* the box the Markdown description goes in */
  GtkLabel *meta;              /* license and author under the title */
  GtkLabel *count;             /* the download count, at the left */

  /* What the dialog of a pack holds while it is up. */
  GPtrArray *versions; /* AmlModrinthVersion* of the details dialog */
  AdwEntryRow *name_entry;
  AdwActionRow *directory_row;
  GtkLabel *details_error; /* inline words when versions fail or are empty */

  /* What the dialog of a pack is waiting for before it shows anything:
     the long description and the versions. */
  gboolean details_ready;
  gboolean versions_ready;
  guint details_generation;  /* rejects answers from older projects */
  AmlModrinthDetails *details; /* owned, the answer read while the dialog is up */
  char *details_problem;       /* why the description is not there */
  char *versions_problem;      /* why the versions are not there */

  /* Widgets of the pages that fall on top of the stack. */
  /* (There is no installing page: the download is the manager's and is
     shown in the header; the dialog closes the moment it starts.) */

  /* The pack download, shown in the header while it runs. Weak: the
     manager owns it, and the pointer goes NULL when it is let go. It is
     the same entry for both phases, the compressed pack and the extract. */
  AmlDownload *pack_download;

  /* What was being retried when an error was shown. */
  gboolean fail_is_install;

  /* Set once the dialog is closed: what is still running must then do
     nothing that needs its widgets. The install carries on, the pack
     download is cancelled. */
  gboolean closed;

  /* The switches of the optional-files page and the files they word;
   * for the question "which of the optional files does the user wish?"
   * Both are reset each visit. */
  GPtrArray *opt_switch_rows; /* AdwSwitchRow*/
  GPtrArray *opt_files;       /* borrowed/self counterpart list */


  /* The running parts. */
  GCancellable *search; /* kills an in-flight search */
  GCancellable *fetch;  /* kills a version fetch, the pack get and install */
  guint debounce;
  guint import_idle;
  AmlTaskScope *scope;
  GPtrArray *rows;
  GPtrArray *hits;

  char *directory;     /* where the instance goes, NULL for the default */
  char *instance_name; /* what the user typed on the details page */
  char *icon_path;     /* the fetched icon of the pack, NULL when it has
                          none; the new instance is given it */
};


void operation_done (gpointer user_data);
GtkWidget *build_error_page (AmlDownloadModpackDialog *, const char *,
                             const char *, GCallback, GCallback);
void push_error (AmlDownloadModpackDialog *, const char *, const char *,
                 GCallback);
void dialog_details_show_again (AmlDownloadModpackDialog *);
void on_pack_parsed (GObject *, GAsyncResult *, gpointer);
gboolean dialog_start_features (AmlDownloadModpackDialog *);
void start_install (AmlDownloadModpackDialog *);
void remove_pack_file (AmlDownloadModpackDialog *);
void on_error_back (GtkButton *, gpointer);
void on_error_retry_fetch (GtkButton *, gpointer);
void on_error_retry_install (GtkButton *, gpointer);

#endif
