/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_CONTENT_PAGE_PRIVATE_H
#define comfy_CONTENT_PAGE_PRIVATE_H

#include "comfy-content-page.h"
#include "comfy-content-browser-model.h"
#include "comfy-content-install-model.h"

G_BEGIN_DECLS

typedef struct {
  ComfyContentPage *page; /* weak */

  /* The operation this watch was made for, when the caller started one.
     Released with the watch, so the answer never outlives its handle. */
  ComfyTaskScope *operation;
} PageWatch;

typedef struct _Pick Pick;
void pick_free (Pick *pick);
void batch_next (ComfyContentPage *self);
void batch_stopped (ComfyContentPage *self);
void update_batch_next (ComfyContentPage *self);
void plan_stopped (ComfyContentPage *self);

PageWatch *page_watch_new (ComfyContentPage *self);
void page_watch_free (PageWatch *watch);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PageWatch, page_watch_free)
void take_rows_out (GList **rows, AdwPreferencesGroup *group);
void file_row_refresh (GtkWidget *row);
char *description_line (const char *text, guint max_sentences);
void row_show_description (GtkWidget *row, const char *description);
void reload_installed (ComfyContentPage *self);
void comfy_content_page_on_installed_scroll_changed (GtkAdjustment *adjustment,
                                                   gpointer user_data);

struct _ComfyContentPage
{
  GtkBox parent_instance;

  ComfyInstance *instance; /* own reference */
  ComfyContentType type;
  char *minecraft_version;
  char *loader; /* NULL for the content that needs none */
  char *folder; /* the folder of the instance this content lives in */

  /* Borrowed from the widget tree of the page, and cleared in dispose()
   * because a request can answer after the tree is gone. */
  AdwToastOverlay *toasts;
  AdwBanner *banner; /* what a failure is said in, until it is read */
  AdwViewStack *stack;

  /* The row that decides between Installed and Browse: the actions of
   * the list being shown on the left, the switcher on the right. Only
   * one of the two action boxes is a child of it at a time. */
  GtkBox *bar;
  GtkBox *bar_spacer; /* takes the room between the actions and the
                         switcher, so the switcher sits at the right */
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
    GPtrArray *files;     /* ComfyContentFile* */
    GHashTable *rows;     /* path -> row */
    GList *rows_list;     /* rows of the Installed group */
    GCancellable *scan;
    gboolean loaded;
    gboolean identifying;
  } installed;
  GtkBox *browse_actions;

  ComfyContentInstallModel *install_model;

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
    ComfyContentBrowserModel *model;
  } browse;

  /* Browse state is grouped below. */


  /* State for the currently open project and its details page. */
  struct {
    gboolean details_ready;
    gboolean versions_ready;
    ComfyModrinthDetails *details;         /* owned */
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
};


/* Internal page-construction callbacks shared with the UI module. */
void comfy_content_page_build_installed (ComfyContentPage *self);
void comfy_content_page_build_browse (ComfyContentPage *self);
void comfy_content_page_build_bar (ComfyContentPage *self);
void comfy_content_page_move_actions_into_bar (ComfyContentPage *self,
                                             gboolean browsing);

void comfy_content_page_on_installed_search_changed (GtkEditable *editable,
                                                   gpointer user_data);
void comfy_content_page_on_update_all_clicked (GtkButton *button,
                                             gpointer user_data);
void comfy_content_page_on_open_folder_clicked (GtkButton *button,
                                              gpointer user_data);
void comfy_content_page_on_search_changed (GtkEditable *editable,
                                         gpointer user_data);
gboolean comfy_content_page_on_search_key (GtkEventControllerKey *controller,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer user_data);
void comfy_content_page_on_filter_clicked (GtkWidget *popover,
                                         gpointer user_data);
void comfy_content_page_on_pick_button_clicked (GtkButton *button,
                                              gpointer user_data);
void comfy_content_page_on_retry_page_clicked (GtkButton *button,
                                             gpointer user_data);
void comfy_content_page_on_scroll_changed (GtkAdjustment *adjustment,
                                         gpointer user_data);
gboolean comfy_content_page_on_content_search_key (GtkEventControllerKey *controller,
                                                 guint keyval, guint keycode,
                                                 GdkModifierType state,
                                                 gpointer user_data);
void comfy_content_page_refresh_picks (ComfyContentPage *self);
void comfy_content_page_clear_picks (ComfyContentPage *self);
void comfy_content_page_close_detail (ComfyContentPage *self);
void comfy_content_page_reset_for_tab_switch (ComfyContentPage *self);
void comfy_content_page_run_search (ComfyContentPage *self, gboolean append);
void comfy_content_page_on_stack_switched (GObject *stack, GParamSpec *pspec,
                                         gpointer user_data);
void on_banner_dismiss (AdwBanner *banner, gpointer user_data);
void set_busy (ComfyContentPage *self, gboolean busy);
void show_toast (ComfyContentPage *self, const char *message);
GtkWidget *plain_label (const char *text);
void watch_widget (gpointer field, GtkWidget *widget);
void start_plan (ComfyContentPage *self, ComfyModrinthVersion *version,
                 const char *title, const char *replaces, gboolean disabled);
ComfyContentFile *installed_project_file (ComfyContentPage *self,
                                       const char *project_id);
void remove_duplicate_project_files (ComfyContentPage *self,
                                     const char *project_id,
                                     const char *keep_path);
void show_error (ComfyContentPage *self, const char *what, GError *error);
void comfy_content_page_on_result_activated (AdwActionRow *row, gpointer user_data);
void open_project_page (ComfyContentPage *self, ComfyModrinthProject *project);
ComfyModrinthVersion *selected_version (GtkDropDown *dropdown,
                                      GPtrArray *versions);
const char *version_name_of (ComfyModrinthVersion *version);
void pick_add (ComfyContentPage *self, ComfyModrinthVersion *version);
void pick_remove (ComfyContentPage *self, ComfyModrinthVersion *version);
void refresh_pick_check (ComfyContentPage *self);

G_END_DECLS

#endif /* comfy_CONTENT_PAGE_PRIVATE_H */
