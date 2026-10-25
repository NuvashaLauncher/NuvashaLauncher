/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_CONTENT_PAGE_PRIVATE_H
#define nuvasha_CONTENT_PAGE_PRIVATE_H

#include "nuvasha-content-page.h"
#include "nuvasha-content-browser-model.h"
#include "nuvasha-content-install-model.h"

G_BEGIN_DECLS

typedef struct {
  NuvashaContentPage *page; /* weak */

  /* The operation this watch was made for, when the caller started one.
     Released with the watch, so the answer never outlives its handle. */
  NuvashaTaskScope *operation;
} PageWatch;

typedef struct _Pick Pick;
void pick_free (Pick *pick);
void batch_next (NuvashaContentPage *self);
void batch_stopped (NuvashaContentPage *self);
void update_batch_next (NuvashaContentPage *self);
void plan_stopped (NuvashaContentPage *self);

PageWatch *page_watch_new (NuvashaContentPage *self);
void page_watch_free (PageWatch *watch);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PageWatch, page_watch_free)
void take_rows_out (GList **rows, AdwPreferencesGroup *group);
void file_row_refresh (GtkWidget *row);
void content_page_toggle_edit (GtkButton *button, gpointer user_data);
void content_page_update_selection (NuvashaContentPage *self);
void content_page_bulk_delete (GtkButton *button, gpointer user_data);
void installed_filter_rows (NuvashaContentPage *self);
GtkWidget *file_row (NuvashaContentPage *self, NuvashaContentFile *file);
void load_visible_icons (NuvashaContentPage *self);
void persist_identified_file (NuvashaContentPage *self,
                              NuvashaContentFile *file);
char *description_line (const char *text, guint max_sentences);
void row_show_description (GtkWidget *row, const char *description);
void reload_installed (NuvashaContentPage *self);
void nuvasha_content_page_on_installed_scroll_changed (GtkAdjustment *adjustment,
                                                   gpointer user_data);

struct _NuvashaContentPage
{
  GtkBox parent_instance;

  NuvashaInstance *instance; /* own reference */
  NuvashaContentType type;
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
    AdwPreferencesGroup *group;
    GtkScrolledWindow *scroller;
    AdwStatusPage *status;
    AdwSpinner *loading;
    GtkButton *update_all;
    GtkStack *update_content;
    AdwSpinner *update_spinner;
    GPtrArray *files;     /* NuvashaContentFile* */
    GHashTable *rows;     /* path -> row */
    GList *rows_list;     /* rows of the Installed group */
    GCancellable *scan;
    gboolean loaded;
    gboolean identifying;
    GtkButton *edit_button;
    GtkActionBar *selection_footer;
    GtkLabel *selection_label;
    GtkButton *selection_delete;
    GHashTable *selected_paths; /* path -> TRUE */
    gboolean edit_mode;
  } installed;
  GtkBox *browse_actions;

  NuvashaContentInstallModel *install_model;

  struct {
    AdwPreferencesGroup *results_group;
    GtkScrolledWindow *results_scroller;
    AdwStatusPage *results_status;
    GtkSearchEntry *search_entry;
    GtkSearchEntry *search_bar;
    GtkBox *filter_box;
    GtkMenuButton *filter_button;
    GtkBox *loading_more; /* spinner row at the bottom of the list */
    GtkBox *retry_row;    /* inline retry row when a page fails */
    GtkLabel *retry_label;
    AdwSpinner *spinner;

    GList *result_rows;
    NuvashaContentBrowserModel *model;
  } browse;

  /* Browse state is grouped below. */


  /* State for the currently open project and its details page. */
  struct {
    gboolean details_ready;
    gboolean versions_ready;
    NuvashaModrinthDetails *details;         /* owned */
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
void nuvasha_content_page_build_installed (NuvashaContentPage *self);
void nuvasha_content_page_build_browse (NuvashaContentPage *self);
void nuvasha_content_page_build_bar (NuvashaContentPage *self);
void nuvasha_content_page_move_actions_into_bar (NuvashaContentPage *self,
                                             gboolean browsing);

void nuvasha_content_page_on_installed_search_changed (GtkEditable *editable,
                                                   gpointer user_data);
void nuvasha_content_page_on_update_all_clicked (GtkButton *button,
                                             gpointer user_data);
void nuvasha_content_page_on_open_folder_clicked (GtkButton *button,
                                              gpointer user_data);
void nuvasha_content_page_on_search_changed (GtkEditable *editable,
                                         gpointer user_data);
gboolean nuvasha_content_page_on_search_key (GtkEventControllerKey *controller,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer user_data);
void nuvasha_content_page_on_filter_clicked (GtkWidget *popover,
                                         gpointer user_data);
void nuvasha_content_page_on_pick_button_clicked (GtkButton *button,
                                              gpointer user_data);
void nuvasha_content_page_on_retry_page_clicked (GtkButton *button,
                                             gpointer user_data);
void nuvasha_content_page_on_scroll_changed (GtkAdjustment *adjustment,
                                         gpointer user_data);
gboolean nuvasha_content_page_on_content_search_key (GtkEventControllerKey *controller,
                                                 guint keyval, guint keycode,
                                                 GdkModifierType state,
                                                 gpointer user_data);
void nuvasha_content_page_refresh_picks (NuvashaContentPage *self);
void nuvasha_content_page_clear_picks (NuvashaContentPage *self);
void nuvasha_content_page_close_detail (NuvashaContentPage *self);
void nuvasha_content_page_reset_for_tab_switch (NuvashaContentPage *self);
void nuvasha_content_page_run_search (NuvashaContentPage *self, gboolean append);
void nuvasha_content_page_on_stack_switched (GObject *stack, GParamSpec *pspec,
                                         gpointer user_data);
void on_banner_dismiss (AdwBanner *banner, gpointer user_data);
void set_busy (NuvashaContentPage *self, gboolean busy);
void show_toast (NuvashaContentPage *self, const char *message);
GtkWidget *plain_label (const char *text);
void watch_widget (gpointer field, GtkWidget *widget);
void start_plan (NuvashaContentPage *self, NuvashaModrinthVersion *version,
                 const char *title, const char *replaces, gboolean disabled);
NuvashaContentFile *installed_project_file (NuvashaContentPage *self,
                                       const char *project_id);
void remove_duplicate_project_files (NuvashaContentPage *self,
                                     const char *project_id,
                                     const char *keep_path);
void show_error (NuvashaContentPage *self, const char *what, GError *error);
void nuvasha_content_page_on_result_activated (AdwActionRow *row, gpointer user_data);
void open_project_page (NuvashaContentPage *self, NuvashaModrinthProject *project);
NuvashaModrinthVersion *selected_version (GtkDropDown *dropdown,
                                      GPtrArray *versions);
const char *version_name_of (NuvashaModrinthVersion *version);
void pick_add (NuvashaContentPage *self, NuvashaModrinthVersion *version);
void pick_remove (NuvashaContentPage *self, NuvashaModrinthVersion *version);
void refresh_pick_check (NuvashaContentPage *self);

G_END_DECLS

#endif /* nuvasha_CONTENT_PAGE_PRIVATE_H */
