/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_CONTENT_PAGE_PRIVATE_H
#define nuvashalauncher_CONTENT_PAGE_PRIVATE_H

#include "nuvashalauncher-content-page.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-content-browser-model.h"
#include "nuvashalauncher-content-install-model.h"

G_BEGIN_DECLS

typedef struct {
  nuvashalauncherContentPage *page; /* weak */

  /* The operation this watch was made for, when the caller started one.
     Released with the watch, so the answer never outlives its handle. */
  nuvashalauncherTaskScope *operation;
} PageWatch;

typedef struct _Pick Pick;
void pick_free (Pick *pick);
void batch_next (nuvashalauncherContentPage *self);
void batch_stopped (nuvashalauncherContentPage *self);
void update_batch_next (nuvashalauncherContentPage *self);
void plan_stopped (nuvashalauncherContentPage *self);

PageWatch *page_watch_new (nuvashalauncherContentPage *self);
void page_watch_free (PageWatch *watch);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PageWatch, page_watch_free)
void take_rows_out (GList **rows, AdwPreferencesGroup *group);
void file_row_refresh (GtkWidget *row);
void content_page_toggle_edit (GtkButton *button, gpointer user_data);
void content_page_update_selection (nuvashalauncherContentPage *self);
void content_page_bulk_delete (GtkButton *button, gpointer user_data);
void installed_filter_rows (nuvashalauncherContentPage *self);
GtkWidget *file_row (nuvashalauncherContentPage *self, nuvashalauncherContentFile *file);
void load_visible_icons (nuvashalauncherContentPage *self);
void persist_identified_file (nuvashalauncherContentPage *self,
                              nuvashalauncherContentFile *file);
char *description_line (const char *text, guint max_sentences);
void row_show_description (GtkWidget *row, const char *description);
void reload_installed (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_on_installed_scroll_changed (GtkAdjustment *adjustment,
                                                   gpointer user_data);

struct _nuvashalauncherContentPage
{
  GtkBox parent_instance;

  nuvashalauncherInstance *instance; /* own reference */
  nuvashalauncherContentType type;
  char *minecraft_version;
  char *loader; /* NULL for the content that needs none */
  char *folder; /* the folder of the instance this content lives in */

  /* Borrowed widget-tree references; cleared in dispose() before async
   * callbacks can use them. */
  AdwToastOverlay *toasts;
  AdwBanner *banner; /* failure message */
  AdwViewStack *stack;

  /* Toolbar containing the active list actions and view switcher. */
  GtkBox *bar;
  GtkBox *bar_spacer; /* expands between actions and switcher */
  GtkBox *switcher;
  GtkToggleButton *switcher_installed; /* the Installed button of the pair */
  GtkToggleButton *switcher_browse;    /* the Browse button of the pair */
  struct {
    GtkBox *actions;
    GtkSearchEntry *search_bar;
    GtkSearchEntry *search_entry;
    GtkButton *search_button;
    GtkBox *search_spacer;
    AdwPreferencesGroup *group;
    GtkScrolledWindow *scroller;
    AdwStatusPage *status;
    AdwSpinner *loading;
    GtkButton *update_all;
    GtkStack *update_content;
    AdwSpinner *update_spinner;
    GtkPopover *update_popover;
    GtkLabel *update_phase;
    GtkLabel *update_count;
    GtkProgressBar *update_bar;
    GtkButton *update_cancel;
    GPtrArray *files;     /* nuvashalauncherContentFile* */
    GHashTable *rows;     /* path -> row */
    GList *rows_list;     /* rows of the Installed group */
    GCancellable *scan;
    gboolean loaded;
    gboolean identifying;
    GtkActionBar *selection_footer;
    GtkLabel *selection_label;
    GtkButton *selection_delete;
    GHashTable *selected_paths; /* path -> TRUE */
  } installed;
  GtkBox *browse_actions;

  nuvashalauncherContentInstallModel *install_model;

  struct {
    AdwPreferencesGroup *results_group;
    GtkScrolledWindow *results_scroller;
    AdwStatusPage *results_status;
    GtkEntry *search_entry;
    GtkEntry *search_bar;
    GtkBox *filter_box;
    GtkPopover *filter_popover;
    GtkBox *loading_more; /* spinner row at the bottom of the list */
    GtkBox *retry_row;    /* inline retry row when a page fails */
    GtkLabel *retry_label;
    AdwSpinner *spinner;

    GList *result_rows;
    nuvashalauncherContentBrowserModel *model;
  } browse;

  /* Browse state is grouped below. */


  /* State for the currently open project and its details page. */
  struct {
    gboolean details_ready;
    gboolean versions_ready;
    nuvashalauncherModrinthDetails *details;         /* owned */
    GPtrArray *details_versions;         /* owned */
    char *details_problem;
    char *versions_problem;
    char *current_project_title;
    char *current_project_id;
    guint64 current_project_downloads;
    char *details_author;
    GtkBox *details_body;
    GtkDropDown *details_dropdown;
    GtkButton *details_download;
    AdwNavigationPage *details_page;     /* weak */
    AdwNavigationSplitView *split;
    GtkCheckButton *details_check;
  } project;

  GtkButton *pick_button;
  GtkLabel *pick_badge;
  GtkActionBar *pick_footer;
};


/* Internal page-construction callbacks shared with the UI module. */
void nuvashalauncher_content_page_build_installed (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_set_installed_search_visible (
  nuvashalauncherContentPage *self, gboolean visible);
void nuvashalauncher_content_page_build_browse (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_build_bar (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_move_actions_into_bar (nuvashalauncherContentPage *self,
                                             gboolean browsing);

void nuvashalauncher_content_page_on_installed_search_changed (GtkEditable *editable,
                                                   gpointer user_data);
void nuvashalauncher_content_page_on_update_all_clicked (GtkButton *button,
                                                        gpointer user_data);
void nuvashalauncher_content_page_on_update_cancel (GtkButton *button,
                                            gpointer user_data);
void nuvashalauncher_content_page_on_open_folder_clicked (GtkButton *button,
                                              gpointer user_data);
void nuvashalauncher_content_page_on_search_changed (GtkEditable *editable,
                                         gpointer user_data);
gboolean nuvashalauncher_content_page_on_search_key (GtkEventControllerKey *controller,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer user_data);
void nuvashalauncher_content_page_on_filter_clicked (GtkWidget *popover,
                                          gpointer user_data);
void nuvashalauncher_content_page_on_pick_button_clicked (GtkButton *button,
                                              gpointer user_data);
void nuvashalauncher_content_page_on_retry_page_clicked (GtkButton *button,
                                             gpointer user_data);
void nuvashalauncher_content_page_on_scroll_changed (GtkAdjustment *adjustment,
                                         gpointer user_data);
gboolean nuvashalauncher_content_page_on_content_search_key (GtkEventControllerKey *controller,
                                                 guint keyval, guint keycode,
                                                 GdkModifierType state,
                                                 gpointer user_data);
void nuvashalauncher_content_page_refresh_picks (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_clear_picks (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_close_detail (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_reset_for_tab_switch (nuvashalauncherContentPage *self);
void nuvashalauncher_content_page_run_search (nuvashalauncherContentPage *self, gboolean append);
void nuvashalauncher_content_page_on_stack_switched (GObject *stack, GParamSpec *pspec,
                                         gpointer user_data);
void on_banner_dismiss (AdwBanner *banner, gpointer user_data);
void set_busy (nuvashalauncherContentPage *self, gboolean busy);
void show_toast (nuvashalauncherContentPage *self, const char *message);
GtkWidget *plain_label (const char *text);
void watch_widget (gpointer field, GtkWidget *widget);
void start_plan (nuvashalauncherContentPage *self, nuvashalauncherModrinthVersion *version,
                 const char *title, const char *replaces, gboolean disabled);
nuvashalauncherContentFile *installed_project_file (nuvashalauncherContentPage *self,
                                       const char *project_id);
void remove_duplicate_project_files (nuvashalauncherContentPage *self,
                                     const char *project_id,
                                     const char *keep_path);
void show_error (nuvashalauncherContentPage *self, const char *what, GError *error);
void nuvashalauncher_content_page_on_result_activated (AdwActionRow *row, gpointer user_data);
void open_project_page (nuvashalauncherContentPage *self, nuvashalauncherModrinthProject *project);
nuvashalauncherModrinthVersion *selected_version (GtkDropDown *dropdown,
                                      GPtrArray *versions);
const char *version_name_of (nuvashalauncherModrinthVersion *version);
void pick_add (nuvashalauncherContentPage *self, nuvashalauncherModrinthVersion *version);
void pick_remove (nuvashalauncherContentPage *self, nuvashalauncherModrinthVersion *version);
void refresh_pick_check (nuvashalauncherContentPage *self);

G_END_DECLS

#endif /* nuvashalauncher_CONTENT_PAGE_PRIVATE_H */
