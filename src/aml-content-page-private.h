/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_CONTENT_PAGE_PRIVATE_H
#define AML_CONTENT_PAGE_PRIVATE_H

#include "aml-content-page.h"

G_BEGIN_DECLS

typedef struct {
  AmlContentPage *page; /* weak */
} PageWatch;

typedef struct _Pick Pick;
void pick_free (Pick *pick);
void batch_next (AmlContentPage *self);
void batch_stopped (AmlContentPage *self);
void update_batch_next (AmlContentPage *self);
void plan_stopped (AmlContentPage *self);

PageWatch *page_watch_new (AmlContentPage *self);
void page_watch_free (PageWatch *watch);
void take_rows_out (GList **rows, AdwPreferencesGroup *group);
void file_row_refresh (GtkWidget *row);
char *description_line (const char *text, guint max_sentences);
void row_show_description (GtkWidget *row, const char *description);
void installed_icon_lookup (AmlContentPage *self, GtkWidget *row,
                            const char *project_id);

struct _AmlContentPage
{
  GtkBox parent_instance;

  AmlInstance *instance; /* own reference */
  AmlContentType type;
  char *minecraft_version;
  char *loader; /* NULL for the content that needs none */
  char *folder; /* the folder of the instance this content lives in */

  /* Borrowed from the widget tree of the page, and cleared in dispose()
   * because a request can answer after the tree is gone. */
  GtkWidget *toasts;
  GtkWidget *banner; /* what a failure is said in, until it is read */
  GtkWidget *stack;

  /* The row that decides between Installed and Browse: the actions of
   * the list being shown on the left, the switcher on the right. Only
   * one of the two action boxes is a child of it at a time. */
  GtkWidget *bar;
  GtkWidget *bar_spacer; /* takes the room between the actions and the
                            switcher, so the switcher sits at the right */
  GtkWidget *switcher;
  GtkWidget *switcher_installed; /* the Installed button of the pair */
  GtkWidget *switcher_browse;    /* the Browse button of the pair */
  struct {
    GtkWidget *actions;
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *group;
    GtkWidget *scroller;
    GtkWidget *status;
    GtkWidget *update_all;
    GPtrArray *files;     /* AmlContentFile* */
    GHashTable *rows;     /* path -> row */
    GList *rows_list;     /* rows of the Installed group */
    GCancellable *scan;
    gboolean identifying;
  } installed;
  GtkWidget *browse_actions;

  /* Update All and selected-version installs share this workflow state. */
  struct {
    gboolean updating;
    GPtrArray *updates; /* UpdateItem* */
    guint update_index;
    GPtrArray *picks;   /* Pick* */
    GPtrArray *batch;   /* Pick*, the list being installed */
    gboolean picking;   /* the batch is being installed now */
    guint pick_index;
  } install;

  struct {
    GtkWidget *results_group;
    GtkWidget *results_scroller;
    GtkWidget *results_status;
    GtkWidget *search_entry;
    GtkWidget *search_bar;
    GtkWidget *filter_box;
    GtkWidget *loading_more; /* spinner row at the bottom of the list */
    GtkWidget *retry_row;    /* inline retry row when a page fails */
    GtkWidget *retry_label;
    GtkWidget *spinner;

    GList *result_rows;
    GHashTable *projects;
    GHashTable *project_icons;
    GHashTable *project_descriptions;
    GHashTable *project_titles;
    char *query;
    GPtrArray *categories;
    GPtrArray *results;
    guint offset;
    guint64 total_hits;
    gboolean categories_loaded;
    gboolean searched;
    gboolean loading_page;
    gboolean pending_append;
    GHashTable *seen_ids;
    GCancellable *more;
    guint debounce_source;
    GCancellable *cancellable;
  } browse;

  /* Browse state is grouped below. */


  /* State for the currently open project and its details page. */
  struct {
    gboolean details_ready;
    gboolean versions_ready;
    AmlModrinthDetails *details;         /* owned */
    GPtrArray *details_versions;         /* owned */
    char *details_problem;
    char *versions_problem;
    char *current_project_title;
    char *current_project_id;
    guint64 current_project_downloads;
    char *details_author;
    GtkWidget *details_body;
    GtkWidget *details_dropdown;
    GtkWidget *details_download;
    GtkWidget *details_page;             /* weak */
    GtkWidget *split;
    GtkWidget *details_check;
  } project;

  GtkWidget *pick_button;
  GtkWidget *pick_badge;
};


/* Internal page-construction callbacks shared with the UI module. */
void aml_content_page_build_installed (AmlContentPage *self);
void aml_content_page_build_browse (AmlContentPage *self);
void aml_content_page_build_bar (AmlContentPage *self);
void aml_content_page_move_actions_into_bar (AmlContentPage *self,
                                             gboolean browsing);

void aml_content_page_on_installed_search_changed (GtkEditable *editable,
                                                   gpointer user_data);
void aml_content_page_on_update_all_clicked (GtkButton *button,
                                             gpointer user_data);
void aml_content_page_on_open_folder_clicked (GtkButton *button,
                                              gpointer user_data);
void aml_content_page_on_search_changed (GtkEditable *editable,
                                         gpointer user_data);
gboolean aml_content_page_on_search_key (GtkEventControllerKey *controller,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer user_data);
void aml_content_page_on_filter_clicked (GtkWidget *popover,
                                         gpointer user_data);
void aml_content_page_on_pick_button_clicked (GtkButton *button,
                                              gpointer user_data);
void aml_content_page_on_retry_page_clicked (GtkButton *button,
                                             gpointer user_data);
void aml_content_page_on_scroll_changed (GtkAdjustment *adjustment,
                                         gpointer user_data);
gboolean aml_content_page_on_content_search_key (GtkEventControllerKey *controller,
                                                 guint keyval, guint keycode,
                                                 GdkModifierType state,
                                                 gpointer user_data);
void aml_content_page_refresh_picks (AmlContentPage *self);
void aml_content_page_clear_picks (AmlContentPage *self);
void aml_content_page_close_detail (AmlContentPage *self);
void aml_content_page_run_search (AmlContentPage *self, gboolean append);
void aml_content_page_on_stack_switched (GObject *stack, GParamSpec *pspec,
                                         gpointer user_data);
void on_banner_dismiss (AdwBanner *banner, gpointer user_data);
void set_busy (AmlContentPage *self, gboolean busy);
void show_toast (AmlContentPage *self, const char *message);
GtkWidget *plain_label (const char *text);
void watch_widget (GtkWidget **field, GtkWidget *widget);
void start_plan (AmlContentPage *self, AmlModrinthVersion *version,
                 const char *title, const char *replaces, gboolean disabled);
void reload_installed (AmlContentPage *self);
AmlContentFile *installed_project_file (AmlContentPage *self,
                                       const char *project_id);
void remove_duplicate_project_files (AmlContentPage *self,
                                     const char *project_id,
                                     const char *keep_path);
void show_error (AmlContentPage *self, const char *what, GError *error);
void aml_content_page_on_result_activated (AdwActionRow *row, gpointer user_data);
AmlModrinthVersion *selected_version (GtkWidget *dropdown,
                                      GPtrArray *versions);
const char *version_name_of (AmlModrinthVersion *version);
void pick_add (AmlContentPage *self, AmlModrinthVersion *version);
void pick_remove (AmlContentPage *self, AmlModrinthVersion *version);
void refresh_pick_check (AmlContentPage *self);

G_END_DECLS

#endif /* AML_CONTENT_PAGE_PRIVATE_H */
