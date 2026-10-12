/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlDownloadModpackDialog: browsing Modrinth for modpacks and putting
 * one into a new instance.
 *
 * The browser is a split view: the list of packs on the left, the pack
 * that is opened on the right, and the list alone until one is. The
 * pages that fall on top of it (installing, an error, the pack's
 * optional files) cover the whole dialog and leave the open pack in its
 * half underneath. Nothing the pack needs for its Minecraft version or
 * its loader is asked of the user: both come from what the pack says
 * about itself. */

#include "aml-download-modpack-dialog.h"
#include "aml-content-format.h"

#include "aml-dialogs.h"
#include "aml-downloads.h"
#include "aml-download.h"
#include "aml-i18n.h"
#include "aml-icons.h"
#include "aml-markdown.h"
#include "aml-modpack.h"
#include "aml-modrinth.h"
#include "aml-task.h"
#include "aml-fs.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

/* One page of results. */
#define RESULTS_PER_PAGE 20

/* How long the words of a search entry rest before they are asked for,
 * so that typing is not one request per letter. */
#define SEARCH_DELAY_MS 500

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

G_DEFINE_FINAL_TYPE (AmlDownloadModpackDialog, aml_download_modpack_dialog,
                     ADW_TYPE_DIALOG)

static void run_search (AmlDownloadModpackDialog *self, gboolean append);
static void set_busy (AmlDownloadModpackDialog *self, gboolean busy);
static void on_search_done (GObject *, GAsyncResult *, gpointer);
static void build_browse_page (AmlDownloadModpackDialog *self);
static void build_import_page (AmlDownloadModpackDialog *self);
static GtkWidget * build_optional_page (AmlDownloadModpackDialog *self,
                                        GPtrArray *optional_files);
static GtkWidget * build_error_page (AmlDownloadModpackDialog *self,
                                     const char *what,
                                     const char *message,
                                     GCallback retry,
                                     GCallback cancel);
static void start_install (AmlDownloadModpackDialog *self);
static void on_error_back (GtkButton *, gpointer);
static void on_error_retry_fetch (GtkButton *, gpointer);
static void on_error_retry_install (GtkButton *, gpointer);
static void on_optional_cancel (GtkButton *, gpointer);
static void on_optional_install (GtkButton *, gpointer);
static void on_pack_downloaded (GObject *, GAsyncResult *, gpointer);
static void on_pack_parsed (GObject *, GAsyncResult *, gpointer);
static void dialog_details_open (AmlDownloadModpackDialog *self);
static gboolean dialog_start_features (AmlDownloadModpackDialog *self);
static void on_modpack_download_clicked (GtkButton *, gpointer);
static void dialog_details_fill (AmlDownloadModpackDialog *self);
static void close_details (AmlDownloadModpackDialog *self);
static void show_details_error (AmlDownloadModpackDialog *self,
                                const char *message);

/* Points @field at @widget and watches it: when the details page that
 * holds @widget is destroyed, @field becomes NULL before a request that
 * is still running can touch it. */
static void
details_watch (gpointer field, GtkWidget *widget)
{
  GtkWidget **target = field;

  if (*target != NULL)
    g_object_remove_weak_pointer (G_OBJECT (*target), field);

  *target = widget;

  if (widget != NULL)
    g_object_add_weak_pointer (G_OBJECT (widget), field);
}

/* Drops every weak pointer this dialog holds, so a widget that outlives,
 * or is destroyed before, the signals still to come can never be touched
 * through a stale field. */
static void
dialog_forget_widgets (AmlDownloadModpackDialog *self)
{
  self->split = NULL;

  details_watch (&self->details_page, NULL);
  details_watch (&self->dropdown, NULL);
  details_watch (&self->download_button, NULL);
  details_watch (&self->body, NULL);
  details_watch (&self->meta, NULL);
  details_watch (&self->count, NULL);
  details_watch (&self->details_error, NULL);
  details_watch (&self->name_entry, NULL);
}

static void
dialog_dispose (GObject *object)
{
  AmlDownloadModpackDialog *self = AML_DOWNLOAD_MODPACK_DIALOG (object);

  if (self->search != NULL)
    g_cancellable_cancel (self->search);

  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);

  if (self->scope != NULL)
    aml_task_scope_cancel (self->scope);

  dialog_forget_widgets (self);

  /* The pack download is the manager's; only the weak pointer home is
     ours to give back before the field goes away. */
  if (self->pack_download != NULL)
    g_object_remove_weak_pointer (G_OBJECT (self->pack_download),
                                  (gpointer *) &self->pack_download);

  g_clear_object (&self->manager);
  g_clear_object (&self->search);
  g_clear_object (&self->fetch);
  aml_task_scope_unref (self->scope);
  self->scope = NULL;
  g_clear_pointer (&self->project, aml_modrinth_project_free);
  g_clear_pointer (&self->version, aml_modrinth_version_free);
  g_clear_pointer (&self->modpack, aml_modpack_free);
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  g_clear_pointer (&self->details, aml_modrinth_details_free);
  g_free (self->details_problem);
  g_free (self->versions_problem);
  g_clear_pointer (&self->rows, g_ptr_array_unref);
  g_clear_pointer (&self->hits, g_ptr_array_unref);
  g_clear_pointer (&self->opt_switch_rows, g_ptr_array_unref);
  g_clear_pointer (&self->opt_files, g_ptr_array_unref);
  g_clear_pointer (&self->seen_ids, g_hash_table_unref);
  g_free (self->pack_path);
  g_free (self->directory);
  g_free (self->instance_name);
  self->instance_name = NULL;
  g_free (self->icon_path);
  self->icon_path = NULL;

  G_OBJECT_CLASS (aml_download_modpack_dialog_parent_class)->dispose (object);
}

static void
aml_download_modpack_dialog_class_init (AmlDownloadModpackDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  (void) klass;

  object_class->dispose = dialog_dispose;
}

static void
on_dialog_closed (AdwDialog *dialog G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  if (self->import_idle != 0)
    aml_task_scope_remove_source (self->scope, self->import_idle);
  self->import_idle = 0;
  self->closed = TRUE;

  /* Closing does not stop the pack download: it is the manager's, it
     shows in the header, and the install that follows it runs without
     the dialog. Only the optional-files choice, which needs the dialog,
     is skipped for a pack that arrives after it is gone. */
}

static void
aml_download_modpack_dialog_init (AmlDownloadModpackDialog *self)
{
  self->search = g_cancellable_new ();
  self->fetch = g_cancellable_new ();
  self->scope = aml_task_scope_new (NULL, self->search, NULL, NULL);
  self->rows = NULL;
  self->hits = NULL;
  self->versions = NULL;
  self->seen_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          NULL);

  g_signal_connect (self, "closed", G_CALLBACK (on_dialog_closed), self);

  adw_dialog_set_title (ADW_DIALOG (self), _("Download Modpack"));
  aml_dialog_set_standard_size (ADW_DIALOG (self));

  self->nav = ADW_NAVIGATION_VIEW (adw_navigation_view_new ());
  /* The installing page appears in place instead of sliding in: the
     movement is not information, and the pages of this dialog are few
     and shallow enough that an instant change reads cleanly. */
  adw_navigation_view_set_animate_transitions (
    ADW_NAVIGATION_VIEW (self->nav), FALSE);
  adw_dialog_set_child (ADW_DIALOG (self), GTK_WIDGET (self->nav));

}

AmlDownloadModpackDialog *
aml_download_modpack_dialog_new (AmlInstanceManager *manager)
{
  AmlDownloadModpackDialog *self =
    g_object_new (AML_TYPE_DOWNLOAD_MODPACK_DIALOG, NULL);

  self->manager = g_object_ref (manager);
  build_browse_page (self);

  return self;
}

static void
build_import_page (AmlDownloadModpackDialog *self)
{
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  AdwSpinner *spinner = ADW_SPINNER (adw_spinner_new ());
  GtkLabel *label = GTK_LABEL (gtk_label_new (_("Importing modpack…")));
  AdwNavigationPage *page = ADW_NAVIGATION_PAGE (
    adw_navigation_page_new (GTK_WIDGET (box), _("Import Modpack")));

  gtk_widget_set_halign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (spinner));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (label));
  adw_navigation_view_add (ADW_NAVIGATION_VIEW (self->nav),
                           ADW_NAVIGATION_PAGE (page));
}

static gboolean
start_import_idle (gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  aml_task_scope_forget_source (self->scope, self->import_idle);
  self->import_idle = 0;

  /* Reuse the same validation, optional-file selection and install flow as
   * a downloaded pack. The selected file is never deleted on failure. */
  aml_modpack_new_from_zip_async (
    self->pack_path,
    self->imported ? self->fetch
                   : aml_download_get_cancellable (self->pack_download),
    on_pack_parsed, g_object_ref (self));
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

AmlDownloadModpackDialog *
aml_download_modpack_dialog_new_for_file (AmlInstanceManager *manager,
                                          const char *path)
{
  AmlDownloadModpackDialog *self =
    aml_download_modpack_dialog_new (manager);

  self->pack_path = g_strdup (path);
  self->imported = TRUE;
  {
    g_autofree char *name = g_path_get_basename (path);

    self->pack_download = aml_downloads_add (AML_DOWNLOAD_MODPACK, name);
  }
  aml_download_set_detail (self->pack_download, _("Reading modpack"));
  g_object_add_weak_pointer (G_OBJECT (self->pack_download),
                             (gpointer *) &self->pack_download);
  build_import_page (self);
  self->import_idle = g_idle_add (start_import_idle, g_object_ref (self));
  aml_task_scope_track_source (self->scope, self->import_idle);
  return self;
}

/* ---------------------------------------------------------- *
 * The browser page
 * ---------------------------------------------------------- */

static gboolean
on_search_timeout (gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  aml_task_scope_forget_source (self->scope, self->debounce);
  self->debounce = 0;
  run_search (self, FALSE);

  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  if (self->debounce != 0)
    aml_task_scope_remove_source (self->scope, self->debounce);

  self->debounce = g_timeout_add (SEARCH_DELAY_MS, on_search_timeout, self);
  aml_task_scope_track_source (self->scope, self->debounce);
}

/* The list is scrolled to its end: ask for the next page. Only one page
 * is ever on its way and there is nothing to ask for once the total is
 * reached.
 *
 * GTK4 has no edge-reached signal on GtkScrolledWindow, so the adjustment
 * is watched instead. */
static void
on_scroll_changed (GtkAdjustment *adjustment, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  gdouble at_end;

  if (self->loading_page)
    return;

  if ((guint64) self->offset >= self->total)
    return;

  at_end = gtk_adjustment_get_upper (adjustment) -
           gtk_adjustment_get_page_size (adjustment) -
           gtk_adjustment_get_step_increment (adjustment);

  if (gtk_adjustment_get_value (adjustment) < at_end)
    return;

  self->offset += RESULTS_PER_PAGE;
  run_search (self, TRUE);
}

/* A page that failed can be asked for again from the row that says so. */
static void
on_retry_page (AmlDownloadModpackDialog *self)
{
  if (self->loading_page)
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->retry_row), FALSE);
  run_search (self, TRUE);
}

static void
take_rows_out (GPtrArray **rows, AdwPreferencesGroup *group)
{
  if (*rows == NULL)
    {
      *rows = g_ptr_array_new ();
      return;
    }

  for (guint i = 0; i < (*rows)->len; i++)
    adw_preferences_group_remove (group, g_ptr_array_index (*rows, i));

  g_ptr_array_unref (*rows);
  *rows = g_ptr_array_new ();
}

/* A click on a row opens its pack in the other half. A click on the row
 * of the pack that is already open closes it again: the list is the way
 * in and the way out, so no button has to stand over the pack and push
 * its name down. */
static void
result_row_activated (AdwActionRow *row, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  AmlModrinthProject *project =
    g_object_get_data (G_OBJECT (row), "aml-project");

  if (project == NULL)
    return;

  if (self->details_page != NULL && self->project != NULL &&
      g_strcmp0 (self->project->project_id, project->project_id) == 0)
    {
      close_details (self);
      return;
    }

  g_clear_pointer (&self->project, aml_modrinth_project_free);
  g_clear_pointer (&self->version, aml_modrinth_version_free);
  self->project = aml_modrinth_project_copy (project);

  dialog_details_open (self);
}

/* Puts the project icon in @icon when it has been fetched; the
 * placeholder stays when there is none. The widget is held for the length
 * of the fetch, so a row that goes away first is still there to be told. */
static void
modpack_icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  GtkWidget *icon = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = aml_icon_load_finish (result, &error);

  if (path != NULL && !aml_icon_widget_set_file (icon, path))
    g_debug ("the icon for \"%s\" could not be decoded",
             (const char *) g_object_get_data (G_OBJECT (icon),
                                               "aml-icon-url"));

  g_object_unref (icon);
}

/* Kicks off one row's icon and remembers the url on the widget, so a
 * failed decode can name it in the log. */
static void
start_modpack_icon (GtkWidget *icon, const char *url)
{
  g_object_set_data_full (G_OBJECT (icon), "aml-icon-url", g_strdup (url),
                          g_free);
  aml_icon_load_async (url, NULL, modpack_icon_ready, g_object_ref (icon));
}

/* The rows of a page of results. @append is FALSE for a new search, which
 * starts the list over, and TRUE for the next page, which is added to what
 * is already there. */
/* The description as one run of words: a row caps its subtitle at two
 * lines by wrapping, and an explicit blank line in the text would slip a
 * third one past that. Same folding the content rows do. */
static char *
one_line (const char *text)
{
  g_autoptr (GString) out = g_string_new (NULL);
  gboolean space = FALSE;

  if (text == NULL)
    return g_strdup ("");

  for (const char *p = text; *p != '\0'; p++)
    {
      if (g_ascii_isspace (*p))
        {
          space = TRUE;
          continue;
        }

      if (space && out->len > 0)
        g_string_append_c (out, ' ');

      space = FALSE;
      g_string_append_c (out, *p);
    }

  return g_string_free (g_steal_pointer (&out), FALSE);
}

static void
results_arrived (AmlDownloadModpackDialog *self, AmlModrinthResults *results,
                 gboolean append)
{
  guint shown = 0;

  if (!append)
    {
      take_rows_out (&self->rows, ADW_PREFERENCES_GROUP (self->results_group));
      g_clear_pointer (&self->hits, g_ptr_array_unref);
      self->hits = g_ptr_array_new_with_free_func (
          (GDestroyNotify) aml_modrinth_project_free);
      self->total = results != NULL ? results->total_hits : 0;
      g_hash_table_remove_all (self->seen_ids);

      /* A new search starts at the top of the list. */
      {
        GtkWidget *scroller =
          gtk_widget_get_ancestor (GTK_WIDGET (self->results_group),
                                   GTK_TYPE_SCROLLED_WINDOW);

        if (scroller != NULL)
          {
            GtkAdjustment *adjustment =
              gtk_scrolled_window_get_vadjustment (
                GTK_SCROLLED_WINDOW (scroller));

            gtk_adjustment_set_value (adjustment,
                                      gtk_adjustment_get_lower (adjustment));
          }
      }
    }

  if (results != NULL)
    {
      for (guint i = 0; i < results->projects->len; i++)
        {
          AmlModrinthProject *project =
              g_ptr_array_index (results->projects, i);
          GtkWidget *row = NULL;
          GtkWidget *icon = NULL;

          /* The same project is never listed twice, however the pages
             overlap. */
          if (g_hash_table_contains (self->seen_ids, project->project_id))
            continue;

          row = adw_action_row_new ();
          icon = aml_icon_widget_new ("view-list-symbolic");

          g_hash_table_add (self->seen_ids, g_strdup (project->project_id));

          /* The icon, then the name and the description under it: the
             author, the download count, the game version and the loader
             are what the details page is for. Markup off first: the
             title and the description are plain Modrinth text and may
             hold an "&". */
          adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (row)),
                                              FALSE);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (row)),
                                         project->title);
          {
            g_autofree char *line = one_line (project->description);

            adw_action_row_set_subtitle (ADW_ACTION_ROW (GTK_WIDGET (row)), line);
          }
          /* Two lines at most, ending in an ellipsis when the
             description is longer: every row is the same height, and the
             whole description is on the details page. */
          adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (GTK_WIDGET (row)), 2);
          adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (GTK_WIDGET (row)), FALSE);

          gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
          start_modpack_icon (icon, project->icon_url);

          adw_action_row_add_prefix (ADW_ACTION_ROW (GTK_WIDGET (row)), icon);

          /* An AdwActionRow is not activatable by default, so the row
             itself is made one: a click anywhere on it opens the
             details page. */
          gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (GTK_WIDGET (row)), TRUE);

          g_ptr_array_add (self->hits,
                           aml_modrinth_project_copy (project));
          g_object_set_data (G_OBJECT (row), "aml-project",
                             g_ptr_array_index (self->hits,
                                                self->hits->len - 1));
          g_ptr_array_add (self->rows, row);

          g_signal_connect (row, "activated",
                            G_CALLBACK (result_row_activated), self);
          adw_preferences_group_add (
              ADW_PREFERENCES_GROUP (self->results_group), row);
          shown++;
        }
    }

  self->offset += shown;

  /* One state at a time: the list, the empty words, or the error. */
  if (self->results_group != NULL)
    {
      gboolean empty = self->hits == NULL || self->hits->len == 0;

      gtk_stack_set_visible_child_name (
          GTK_STACK (self->results_stack), empty ? "empty" : "results");
    }

  gtk_widget_set_visible (GTK_WIDGET (self->loading_more), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_row), FALSE);

  set_busy (self, FALSE);
  self->loading_page = FALSE;
}

static void
run_search (AmlDownloadModpackDialog *self, gboolean append)
{
  const char *query =
    gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  g_autofree char *escaped_query = NULL;
  g_autofree char *escaped_facets = NULL;
  g_autofree char *url = NULL;

  if (self->search != NULL)
    g_cancellable_cancel (self->search);

  g_clear_object (&self->search);
  self->search = g_cancellable_new ();

  if (self->debounce != 0)
    {
      g_source_remove (self->debounce);
      self->debounce = 0;
    }

  if (!append)
    self->offset = 0;

  self->loading_page = TRUE;
  self->append = append;

  set_busy (self, TRUE);

  /* The first page of a search shows the loading state; a page that is
     added below what is already there shows a spinner at its end. */
  if (!append)
    gtk_stack_set_visible_child_name (GTK_STACK (self->results_stack),
                                      "loading");
  else
    {
      gtk_widget_set_visible (GTK_WIDGET (self->loading_more), TRUE);
      gtk_widget_set_visible (GTK_WIDGET (self->retry_row), FALSE);
    }

  escaped_query =
    g_uri_escape_string (query != NULL ? query : "", NULL, FALSE);
  escaped_facets = g_uri_escape_string ("[[\"project_type:modpack\"]]",
                                        NULL, FALSE);

  /* An empty search asks for the popular packs rather than for
     everything by relevance: a list of what is used most is a better
     place to land on than an arbitrary one. */
  if (query == NULL || *query == '\0')
    url = g_strdup_printf (
      AML_MODRINTH_URL "/search?query=%s&facets=%s&index=downloads"
                       "&offset=%u&limit=%u",
      escaped_query, escaped_facets, self->offset, RESULTS_PER_PAGE);
  else
    url = g_strdup_printf (
      AML_MODRINTH_URL "/search?query=%s&facets=%s&offset=%u&limit=%u",
      escaped_query, escaped_facets, self->offset, RESULTS_PER_PAGE);

  /* The callback holds a reference: closing the dialog while a search
   * is in flight finalizes it, and a callback that then reads self would
   * be reading freed memory. */
  aml_download_json_async (url, self->search, on_search_done,
                           g_object_ref (self));
}

/* Every operation of this dialog hands its callback a reference it gives
   back here: the dialog may be closed, and finalized, while the work is
   still going on. */
static void
operation_done (gpointer user_data)
{
  g_object_unref (user_data);
}

static void
on_search_done (GObject *source G_GNUC_UNUSED,
                GAsyncResult *result,
                gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (AmlModrinthResults) results = NULL;
  const char *answer = NULL;

  parser = aml_download_json_finish (result, &error);

  if (parser == NULL)
    {
      if (!g_error_matches (error, AML_DOWNLOAD_ERROR,
                            AML_DOWNLOAD_ERROR_CANCELLED))
        {
          self->loading_page = FALSE;

          if (self->append)
            {
              /* A page that failed is offered again from the list. */
              gtk_label_set_text (GTK_LABEL (self->retry_label),
                                  error->message);
              gtk_widget_set_visible (GTK_WIDGET (self->loading_more), FALSE);
              gtk_widget_set_visible (GTK_WIDGET (self->retry_row), TRUE);
            }
          else
            {
              g_autofree char *escaped =
                g_markup_escape_text (error->message, -1);

              adw_status_page_set_title (
                ADW_STATUS_PAGE (self->error_status),
                "Could not search Modrinth");
              adw_status_page_set_description (
                ADW_STATUS_PAGE (self->error_status), escaped);
              gtk_stack_set_visible_child_name (
                GTK_STACK (self->results_stack), "error");
            }
        }

      operation_done (user_data);
      return;
    }

  answer = json_to_string (json_parser_get_root (parser), FALSE);
  results = aml_modrinth_results_from_json (answer, -1, &error);

  if (results == NULL)
    {
      self->loading_page = FALSE;

      if (self->append)
        {
          gtk_label_set_text (GTK_LABEL (self->retry_label), error->message);
          gtk_widget_set_visible (GTK_WIDGET (self->loading_more), FALSE);
          gtk_widget_set_visible (GTK_WIDGET (self->retry_row), TRUE);
        }
      else
        {
          g_autofree char *escaped = g_markup_escape_text (error->message, -1);

          adw_status_page_set_title (ADW_STATUS_PAGE (self->error_status),
                                     "Could not understand the answer");
          adw_status_page_set_description (
            ADW_STATUS_PAGE (self->error_status), escaped);
          gtk_stack_set_visible_child_name (
            GTK_STACK (self->results_stack), "error");
        }

      operation_done (user_data);
      return;
    }

  results_arrived (self, results, self->append);
  operation_done (user_data);
}

static void
build_browse_page (AmlDownloadModpackDialog *self)
{
  GtkWidget *page = NULL;
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkBox *top = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  GtkBox *loading = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 6));
  AdwSpinner *loading_spinner = ADW_SPINNER (adw_spinner_new ());
  GtkLabel *loading_label = GTK_LABEL (gtk_label_new (_("Loading modpacks…")));
  GtkBox *loading_more = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  AdwSpinner *more_spinner = ADW_SPINNER (adw_spinner_new ());
  GtkLabel *more_label = GTK_LABEL (gtk_label_new (_("Loading more…")));
  GtkBox *retry_row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkLabel *retry_label = GTK_LABEL (gtk_label_new (NULL));
  GtkButton *retry_button = GTK_BUTTON (gtk_button_new_with_label (_("Retry")));
  GtkButton *close_button = GTK_BUTTON (gtk_button_new_from_icon_name ("window-close-symbolic"));

  /* Exactly one of these is shown at any moment: the list, the words for
     an empty answer, the error, or the spinner while the first page is
     on its way. A status page that stayed under the results was the bug
     this replaces. */
  self->results_stack = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->results_stack), TRUE);

  self->results_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  /* The list starts where the search row above it ends: its first row is
     not rounded at the top, so the two read as one piece. */
  gtk_widget_add_css_class (GTK_WIDGET (self->results_group), "flush");

  self->search_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->search_entry), TRUE);
  gtk_search_entry_set_placeholder_text (
    GTK_SEARCH_ENTRY (self->search_entry), _("Search modpacks"));
  g_signal_connect (self->search_entry, "changed",
                    G_CALLBACK (on_search_changed), self);

  gtk_box_append (GTK_BOX (GTK_WIDGET (top)), GTK_WIDGET (self->search_entry));
  gtk_widget_add_css_class (GTK_WIDGET (close_button), "flat");
  gtk_widget_set_tooltip_text (GTK_WIDGET (close_button), _("Close"));
  g_signal_connect_swapped (close_button, "clicked",
                            G_CALLBACK (adw_dialog_close), self);
  gtk_box_append (GTK_BOX (GTK_WIDGET (top)), GTK_WIDGET (close_button));
  gtk_widget_set_margin_start (GTK_WIDGET (top), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (top), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (top), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (top), 6);
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (top));

  /* The list. The bottom of it holds a spinner while the next page is on
     its way and a retry row when a page failed. */
  gtk_widget_set_halign (GTK_WIDGET (loading_more), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (GTK_WIDGET (loading_more), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (loading_more), 12);
  gtk_widget_set_visible (GTK_WIDGET (loading_more), FALSE);
  gtk_box_append (GTK_BOX (GTK_WIDGET (loading_more)), GTK_WIDGET (more_spinner));
  gtk_box_append (GTK_BOX (GTK_WIDGET (loading_more)), GTK_WIDGET (more_label));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->results_group),
                              GTK_WIDGET (loading_more));

  gtk_widget_set_halign (GTK_WIDGET (retry_row), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (GTK_WIDGET (retry_row), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (retry_row), 12);
  gtk_widget_set_visible (GTK_WIDGET (retry_row), FALSE);
  gtk_label_set_wrap (GTK_LABEL (GTK_WIDGET (retry_label)), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (retry_label), "dim-label");
  gtk_box_append (GTK_BOX (GTK_WIDGET (retry_row)), GTK_WIDGET (retry_label));
  gtk_box_append (GTK_BOX (GTK_WIDGET (retry_row)), GTK_WIDGET (retry_button));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->results_group),
                              GTK_WIDGET (retry_row));

  self->loading_more = GTK_BOX (loading_more);
  self->retry_row = GTK_BOX (retry_row);
  self->retry_label = GTK_LABEL (retry_label);

  g_signal_connect_swapped (retry_button, "clicked",
                            G_CALLBACK (on_retry_page), self);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (GTK_WIDGET (scroller)),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  /* The group is the whole width of the list: a row of the browser has to
     cover it rather than sit centered in a clamp of its own. */
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (GTK_WIDGET (scroller)),
                                 GTK_WIDGET (self->results_group));
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);
  gtk_stack_add_named (GTK_STACK (self->results_stack), GTK_WIDGET (scroller), "results");

  /* An empty answer. */
  self->results_status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->results_status),
                                 "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                             _("Nothing to show yet"));
  adw_status_page_set_description (
    ADW_STATUS_PAGE (self->results_status),
    "No modpack matches what was asked for. Try another word");
  gtk_stack_add_named (GTK_STACK (self->results_stack),
                       GTK_WIDGET (self->results_status), "empty");

  /* A search that could not be made. */
  self->error_status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->error_status),
                                 "dialog-error-symbolic");
  gtk_stack_add_named (GTK_STACK (self->results_stack),
                       GTK_WIDGET (self->error_status), "error");

  /* The first page is on its way. */
  gtk_widget_set_valign (GTK_WIDGET (loading), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (loading), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (GTK_WIDGET (loading)), GTK_WIDGET (loading_spinner));
  gtk_box_append (GTK_BOX (GTK_WIDGET (loading)), GTK_WIDGET (loading_label));
  gtk_stack_add_named (GTK_STACK (self->results_stack), GTK_WIDGET (loading), "loading");

  gtk_stack_set_visible_child_name (GTK_STACK (self->results_stack),
                                    "loading");
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (self->results_stack));

  /* The list asks for the next page when it is scrolled to its end.
     GTK4 has no edge-reached signal, so the adjustment is watched. */
  {
    GtkAdjustment *adjustment =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (GTK_WIDGET (scroller)));

    g_signal_connect (adjustment, "value-changed",
                      G_CALLBACK (on_scroll_changed), self);
  }

  /* The list is the left half; the pack that is opened is the right half,
     empty until one is opened. Collapsed shows one page at a time, which
     is the list alone. */
  self->split = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->split), TRUE);
  /* Named, so the stylesheet keeps the list half the colour of the dialog
     instead of the sidebar colour a split view would give it. */
  gtk_widget_add_css_class (GTK_WIDGET (self->split), "flat-split");
  adw_navigation_split_view_set_sidebar (
    ADW_NAVIGATION_SPLIT_VIEW (self->split),
    adw_navigation_page_new (GTK_WIDGET (box), "Browse Modpacks"));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), TRUE);
  /* Two halves of one width, rather than the narrow column a split view
     gives a sidebar by default. */
  adw_navigation_split_view_set_sidebar_width_fraction (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), 0.5);
  adw_navigation_split_view_set_min_sidebar_width (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), 360);
  adw_navigation_split_view_set_max_sidebar_width (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), 640);

  page = GTK_WIDGET (adw_navigation_page_new (GTK_WIDGET (self->split), ""));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (GTK_WIDGET (page)), "browse");

  adw_navigation_view_add (ADW_NAVIGATION_VIEW (self->nav),
                           ADW_NAVIGATION_PAGE (page));

  /* The page lands on the popular packs rather than on nothing. */
  run_search (self, FALSE);
}

static void
set_busy (AmlDownloadModpackDialog *self, gboolean busy)
{
  (void) self;
  (void) busy;
}

/* ---------------------------------------------------------- *
 * Versions of the details page
 * ---------------------------------------------------------- */

static void
version_row_label (AmlModrinthVersion *version, char *out, guint size)
{
  const char *name = version->version_number != NULL ? version->version_number : version->name;
  const char *game =
      version->game_versions != NULL && version->game_versions->len > 0
          ? g_ptr_array_index (version->game_versions, 0)
          : "?";
  const char *loader =
      version->loaders != NULL && version->loaders->len > 0
          ? g_ptr_array_index (version->loaders, 0)
          : "vanilla";

  g_snprintf (out, size, "%s · %s · %s", name, game, loader);
}

static gint
by_date_desc (gconstpointer a, gconstpointer b)
{
  const AmlModrinthVersion *va = *(const AmlModrinthVersion *const *) a;
  const AmlModrinthVersion *vb = *(const AmlModrinthVersion *const *) b;

  return g_strcmp0 (vb->date_published, va->date_published);
}

static GtkStringList *
build_version_strings (GPtrArray *versions)
{
  GtkStringList *strings = gtk_string_list_new (NULL);

  for (guint i = 0; i < versions->len; i++)
    {
      AmlModrinthVersion *version = g_ptr_array_index (versions, i);
      char label[160];

      version_row_label (version, label, sizeof label);
      gtk_string_list_append (strings, label);
    }

  return strings;
}

static GPtrArray *
versions_of_answer (JsonNode *root, GError **error)
{
  JsonArray *array = NULL;
  GPtrArray *versions = NULL;

  if (!JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error, AML_MODRINTH_ERROR,
                           AML_MODRINTH_ERROR_PARSE,
                           "the answer is not a list of versions");
      return NULL;
    }

  versions = g_ptr_array_new_with_free_func ((GDestroyNotify)
    aml_modrinth_version_free);

  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *element = json_array_get_element (array, i);
      g_autofree char *json = json_to_string (element, FALSE);
      AmlModrinthVersion *version =
        aml_modrinth_version_from_json (json, -1, NULL);

      if (version == NULL)
        continue;

      {
        gboolean is_a_pack = FALSE;

        for (guint j = 0;
             version->files != NULL && j < version->files->len;
             j++)
          {
            AmlModrinthFile *file = g_ptr_array_index (version->files, j);

            if (file->filename != NULL &&
                g_str_has_suffix (file->filename, ".mrpack"))
              is_a_pack = TRUE;
          }

        if (!is_a_pack)
          {
            aml_modrinth_version_free (version);
            continue;
          }
      }

      g_ptr_array_add (versions, version);
    }

  g_ptr_array_sort (versions, by_date_desc);

  return versions;
}

/* ---------------------------------------------------------- *
 * The dialog of a pack
 *
 * The same one the mods, resource packs and shader packs open: the
 * description in Markdown, and a foot with the versions in a drop-down
 * over the button that installs the one shown. A pack becomes an
 * instance of its own rather than a file in one, so the name it takes
 * and where it goes are asked for here too, below the description.
 * ---------------------------------------------------------- */

/* Words that are not Markdown and must read exactly as they are. */
static GtkWidget *
plain_label (const char *text)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (text != NULL ? text : ""));

  gtk_label_set_wrap (GTK_LABEL (GTK_WIDGET (label)), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (GTK_WIDGET (label)), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (GTK_WIDGET (label)), 0);
  gtk_label_set_selectable (GTK_LABEL (GTK_WIDGET (label)), TRUE);

  return GTK_WIDGET (label);
}

/* The words the dialog shows while the description and the versions are
   on their way. */
static GtkWidget *
loading_child (void)
{
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  AdwSpinner *spinner = ADW_SPINNER (adw_spinner_new ());
  GtkLabel *label = GTK_LABEL (gtk_label_new (_("Loading…")));

  gtk_widget_set_valign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (spinner), GTK_ALIGN_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (spinner), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (label), "dim-label");
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (spinner));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (label));

  return GTK_WIDGET (box);
}

/* Closes the open pack, leaving the list the whole dialog again. The row
 * of the pack is the way in and the way out, so nothing has to stand
 * above the pack and push its name down. */
static void
close_details (AmlDownloadModpackDialog *self)
{
  if (self->split == NULL)
    return;

  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), NULL);
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), TRUE);
  details_watch (&self->details_page, NULL);

  /* A half a pack wide is what the list needs; the room the other half
     took goes back to the dialog. */
  adw_dialog_set_content_width (ADW_DIALOG (self), AML_DIALOG_CONTENT_WIDTH);
}

/* Puts @body in the right half of the browser, beside the list, and takes
 * the whole width of the list back to half. A pack shown before is
 * replaced. */
static void
show_details_page (AmlDownloadModpackDialog *self, GtkWidget *body)
{
  AdwNavigationPage *page = ADW_NAVIGATION_PAGE (adw_navigation_page_new (body,
                                                         self->project->title));

  details_watch (&self->details_page, GTK_WIDGET (page));
  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), ADW_NAVIGATION_PAGE (page));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), FALSE);

  /* The pack opens to the side of the list, so the dialog widens by the
     other half to keep both readable. */
  adw_dialog_set_content_width (ADW_DIALOG (self), 1040);
}

/* The author the search answer named. A project answer does not name one:
   it carries the team as the team's project id. */
static const char *
author_of (AmlDownloadModpackDialog *self)
{
  const char *who = self->project != NULL ? self->project->author : NULL;

  if (who == NULL || *who == '\0')
    who = self->details != NULL ? self->details->author : NULL;

  return who != NULL && *who != '\0' ? who : NULL;
}

/* The whole content of the dialog, built in one go from what the answers
   said. Nothing here is filled in as it arrives: the dialog shows a
   spinner until this is called, so the name and the directory appear
   together with the description and the versions. */
static GtkWidget *
details_content (AmlDownloadModpackDialog *self)
{
  GtkWidget *content = NULL;
  GtkWidget *scroller = NULL;
  GtkWidget *box = NULL;
  GtkWidget *page = NULL;
  GtkWidget *heading = NULL;
  GtkWidget *bar = NULL;
  g_autofree char *downloads = NULL;
  const char *who = author_of (self);

  scroller = gtk_scrolled_window_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  heading = adw_preferences_group_new ();

  /* The title, then who wrote it and how often it was taken, the same
     three lines the other contents open with. */
  page = gtk_label_new (self->project->title);
  gtk_label_set_wrap (GTK_LABEL (GTK_WIDGET (page)), TRUE);
  gtk_label_set_xalign (GTK_LABEL (GTK_WIDGET (page)), 0);
  gtk_widget_add_css_class (GTK_WIDGET (page), "title-2");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)), GTK_WIDGET (page));

  {
    g_autofree char *text = NULL;

    text = aml_content_format_author (
      self->details != NULL ? self->details->license : NULL, who);

    details_watch (&self->meta, gtk_label_new (text));
    gtk_label_set_xalign (GTK_LABEL (self->meta), 0);
    gtk_widget_add_css_class (GTK_WIDGET (self->meta), "dim-label");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)), GTK_WIDGET (self->meta));
  }

   downloads = aml_content_format_downloads (self->project->downloads);
  details_watch (&self->count, gtk_label_new (downloads));
  gtk_label_set_xalign (GTK_LABEL (self->count), 0);
  gtk_widget_add_css_class (GTK_WIDGET (self->count), "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)), GTK_WIDGET (self->count));

  /* The long description, in Markdown. */
  details_watch (&self->body, gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));

  if (self->details_problem != NULL)
    gtk_box_append (GTK_BOX (self->body), plain_label (self->details_problem));
  else if (self->details != NULL && self->details->body != NULL &&
           *self->details->body != '\0')
    aml_markdown_render (GTK_BOX (self->body), self->details->body);
  else
    gtk_box_append (GTK_BOX (self->body),
                    plain_label (self->project->description));

  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (heading));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (self->body));

  /* Where a version list that failed to load, or that holds nothing to
     install, says so: it must not be a disabled button with no reason. */
  details_watch (&self->details_error, gtk_label_new (NULL));
  gtk_label_set_wrap (GTK_LABEL (self->details_error), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->details_error), 0);
  gtk_widget_add_css_class (GTK_WIDGET (self->details_error), "dim-label");
  gtk_widget_set_visible (GTK_WIDGET (self->details_error), FALSE);
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (self->details_error));

  gtk_widget_set_margin_top (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (box), 18);
  gtk_widget_set_margin_end (GTK_WIDGET (box), 18);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (GTK_WIDGET (scroller)), GTK_WIDGET (box));
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);

  /* The foot: every version the pack publishes, and the button that
     installs the one shown. The newest is what it shows from the start. */
  details_watch (&self->dropdown, gtk_drop_down_new (NULL, NULL));
  gtk_widget_set_hexpand (GTK_WIDGET (self->dropdown), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (self->dropdown), GTK_ALIGN_CENTER);

  details_watch (&self->download_button, gtk_button_new_with_label (_("Download")));
  gtk_widget_add_css_class (GTK_WIDGET (self->download_button), "suggested-action");
  gtk_widget_set_valign (GTK_WIDGET (self->download_button), GTK_ALIGN_CENTER);
  /* Nothing can be installed before the versions have arrived. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->download_button), FALSE);
  g_signal_connect (self->download_button, "clicked",
                    G_CALLBACK (on_modpack_download_clicked), self);

  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->dropdown));
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->download_button));
  gtk_widget_set_margin_top (bar, 12);
  gtk_widget_set_margin_bottom (bar, 12);
  gtk_widget_set_margin_start (bar, 12);
  gtk_widget_set_margin_end (bar, 12);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (scroller));
  gtk_box_append (GTK_BOX (content),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (content), bar);

  return content;
}

/* Puts the real content in the half once both the description and the
   versions are in. */
static void
dialog_details_fill (AmlDownloadModpackDialog *self)
{
  if (self->details_page == NULL)
    return;
  if (!self->details_ready || !self->versions_ready)
    return;

  adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self->details_page),
                                 details_content (self));

  if (self->versions_problem != NULL)
    show_details_error (self, self->versions_problem);
  else if (self->versions != NULL && self->versions->len == 0)
    show_details_error (self,
                        "This project publishes no pack file AML can "
                        "install.");

  if (self->versions != NULL && self->versions->len > 0)
    {
      GtkStringList *strings = build_version_strings (self->versions);

      gtk_drop_down_set_model (GTK_DROP_DOWN (self->dropdown),
                               G_LIST_MODEL (strings));
      /* The newest version is what a user most often wants, so it is
         what the drop-down shows from the start. */
      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->dropdown), 0);
      g_object_unref (strings);
    }

  if (self->download_button != NULL)
    gtk_widget_set_sensitive (GTK_WIDGET (self->download_button),
                              self->versions != NULL && self->versions->len > 0);
}

/* Get the modpack's list of published versions and show it on the
 * already-pushed details page. */
static void
show_details_error (AmlDownloadModpackDialog *self, const char *message)
{
  if (self->details_error == NULL)
    return;

  gtk_label_set_text (GTK_LABEL (self->details_error),
                      message != NULL ? message : "");
  gtk_widget_set_visible (GTK_WIDGET (self->details_error), message != NULL);
}

static void
on_versions_answer (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  parser = aml_download_json_finish (result, &error);

  if (parser == NULL)
    {
      if (!g_error_matches (error, AML_DOWNLOAD_ERROR,
                            AML_DOWNLOAD_ERROR_CANCELLED))
        {
          g_free (self->versions_problem);
          self->versions_problem =
            g_strdup (error != NULL ? error->message
                                    : "the versions could not be read");
        }

      operation_done (user_data);
      return;
    }

  versions = versions_of_answer (json_parser_get_root (parser), &error);

  if (versions == NULL)
    {
      g_free (self->versions_problem);
      self->versions_problem =
        g_strdup (error != NULL ? error->message : "the versions could not be read");
      operation_done (user_data);
      return;
    }

  g_ptr_array_sort (versions, by_date_desc);

  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = versions;
  versions = NULL;

  self->versions_ready = TRUE;
  dialog_details_fill (self);

  operation_done (user_data);
}

static void dialog_details_show_again (AmlDownloadModpackDialog *self);
static void dialog_details_fill (AmlDownloadModpackDialog *self);

/* The long description of the pack, read once the dialog is up. */
static void
on_details_answer (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthDetails) details = NULL;

  details = aml_modrinth_details_finish (result, &error);

  if (details == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_free (self->details_problem);
          self->details_problem =
            g_strdup (error != NULL ? error->message
                                    : "the pack cannot be read");
        }
    }
  else
    {
      g_clear_pointer (&self->details, aml_modrinth_details_free);
      self->details = g_steal_pointer (&details);
    }

  self->details_ready = TRUE;
  dialog_details_fill (self);

  operation_done (user_data);
}

/* The icon of the project is previewed from the shared icon cache. It is
 * copied into the new instance before that instance points at it: clearing
 * AML's cache must never remove an instance's icon. The reference the load
 * holds keeps the dialog alive until the answer is in. */
static void
project_icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = aml_icon_load_finish (result, &error);

  if (path != NULL)
    {
      g_free (self->icon_path);
      self->icon_path = g_steal_pointer (&path);
    }

  g_object_unref (self);
}

static void
dialog_details_open (AmlDownloadModpackDialog *self)
{
  g_autofree char *escaped_id = NULL;
  g_autofree char *url = NULL;

  if (self->project == NULL)
    return;

  escaped_id = g_uri_escape_string (self->project->project_id, NULL, FALSE);
  url = g_strdup_printf (AML_MODRINTH_URL "/project/%s/version", escaped_id);

  /* Nothing is shown until the description and the versions are both in,
     so the half does not fill itself in piece by piece. */
  self->details_ready = FALSE;
  self->versions_ready = FALSE;
  g_clear_pointer (&self->details, aml_modrinth_details_free);
  g_free (self->details_problem);
  self->details_problem = NULL;
  g_free (self->versions_problem);
  self->versions_problem = NULL;

  show_details_page (self, loading_child ());

  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);

  g_clear_object (&self->fetch);
  self->fetch = g_cancellable_new ();

  /* A stale icon of another project has no business on this one. */
  g_clear_pointer (&self->icon_path, g_free);

  if (self->project->icon_url != NULL && *self->project->icon_url != '\0')
    aml_icon_load_async (self->project->icon_url, self->fetch,
                         project_icon_ready, g_object_ref (self));

  /* The long description, and the versions of the pack beside it. */
  aml_modrinth_details_async (self->project->project_id, self->fetch,
                              on_details_answer, g_object_ref (self));
  aml_download_json_async (url, self->fetch, on_versions_answer,
                           g_object_ref (self));
}


/* ---------------------------------------------------------- *
 * The other pages
 * ---------------------------------------------------------- */

static GtkWidget *
build_error_page (AmlDownloadModpackDialog *self,
                  const char *what,
                  const char *message,
                  GCallback retry,
                  GCallback cancel)
{
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  GtkWidget *box = NULL;
  AdwStatusPage *status = NULL;
  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkButton *retry_btn = GTK_BUTTON (gtk_button_new_with_label (_("Retry")));
  GtkButton *back_btn = GTK_BUTTON (gtk_button_new_with_label (_("Back")));
  GtkWidget *page = NULL;

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)), GTK_WIDGET (header));

  status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (GTK_WIDGET (status)),
                                 "dialog-error-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (GTK_WIDGET (status)), what);
  {
    g_autofree char *escaped =
      g_markup_escape_text (message != NULL ? message : "", -1);

    adw_status_page_set_description (ADW_STATUS_PAGE (GTK_WIDGET (status)), escaped);
  }

  gtk_widget_add_css_class (GTK_WIDGET (retry_btn), "suggested-action");
  g_signal_connect (retry_btn, "clicked", retry, self);
  g_signal_connect (back_btn, "clicked", cancel, self);
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (retry_btn));
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (back_btn));
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_CENTER);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (status));
  gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (buttons));
  gtk_widget_set_vexpand (GTK_WIDGET (status), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (status), GTK_ALIGN_CENTER);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)), GTK_WIDGET (box));
  page = GTK_WIDGET (adw_navigation_page_new (GTK_WIDGET (toolbar), "Could not install"));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (GTK_WIDGET (page)), "error");

  return page;
}

static void
push_error (AmlDownloadModpackDialog *self,
            const char *what,
            const char *message,
            GCallback retry)
{
  GtkWidget *page = build_error_page (self, what, message, retry,
                                      G_CALLBACK (on_error_back));

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                            ADW_NAVIGATION_PAGE (page));
}

/* Handler buttons patched around the pages. */

/* The pack is shown in the half beside the browser rather than as a page,
   so going back to it means taking the page on top off. */
static void
dialog_details_show_again (AmlDownloadModpackDialog *self)
{
  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (self->nav), "browse");
}

static void
on_error_back (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  dialog_details_show_again (self);
}

static void
on_error_retry_fetch (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  self->fail_is_install = FALSE;

  /* Back onto the installing page the failed download left below the
   * error, or onto the details page when there was never one. */
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (self->nav));
  dialog_start_features (self);
}

static void
on_error_retry_install (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  /* Back onto the installing page of the attempt that failed. */
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (self->nav));
  start_install (self);
}

/* The Download button under the versions: it installs the version the
   drop-down shows, and the dialog it was in has done its work. */
static void
on_modpack_download_clicked (GtkButton *button G_GNUC_UNUSED,
                             gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  guint selected = 0;

  if (self->dropdown == NULL || self->versions == NULL)
    return;

  selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->dropdown));

  if (selected == GTK_INVALID_LIST_POSITION || selected >= self->versions->len)
    return;

  g_free (self->instance_name);
  self->instance_name = g_strdup (self->project->title);

  if (self->version != NULL)
    aml_modrinth_version_free (self->version);

  self->version = aml_modrinth_version_copy (
      g_ptr_array_index (self->versions, selected));

  /* The pack download and the install that follows are the manager's:
     the dialog has nothing left to show, so it closes and lets both run
     in the background with the header showing what is on its way. The
     flag is set first, so a fast (cached) pack can never race the close
     and put the optional page back up. */
  if (dialog_start_features (self))
    {
      self->closed = TRUE;
      adw_dialog_close (ADW_DIALOG (self));
    }
}

/* ---------------------------------------------------------- *
 * The pack fetched and the pack installed
 * ---------------------------------------------------------- */

/* What one background install keeps alive: the dialog it was started
 * from (whose widgets may already be gone) and the manager's download.
 * Both are held, so the callbacks can run either while the dialog is up
 * or after it was closed. */
typedef struct {
  AmlDownloadModpackDialog *dialog;
  AmlDownload *download;
  AmlTaskScope *operation;
} InstallJob;

static void
install_job_free (InstallJob *job)
{
  g_clear_object (&job->dialog);
  g_clear_object (&job->download);
  if (job->operation != NULL)
    aml_task_scope_unref (job->operation);
  g_free (job);
}

static void
on_install_progress (guint done,
                     guint total,
  const char *path,
                     gpointer user_data)
{
  InstallJob *job = user_data;

  /* The install's "n of m files" lives in the header's dropdown; a NULL
   * path marks the transition from downloading to extraction. */
  if (path == NULL)
    aml_download_set_detail (job->download, _("Extracting files"));
  aml_download_set_progress (job->download, done, total);
}

/* A downloaded .mrpack is only needed until its overrides are laid down.
 * An imported file belongs to the user and is never removed here. */
static void
remove_pack_file (AmlDownloadModpackDialog *self)
{
  if (!self->imported && self->pack_path != NULL)
    {
      aml_fs_unlink (self->pack_path, NULL);
      g_clear_pointer (&self->pack_path, g_free);
    }
}

static void
on_install_done (GObject *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer user_data)
{
  InstallJob *job = user_data;
  AmlDownloadModpackDialog *self = job->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlInstance) instance = NULL;

  instance = aml_modpack_install_finish (result, &error);

  /* The entry leaves the header's dropdown whether it worked or not, and
     the compressed pack it came from goes with it. */
  aml_download_finish (job->download);
  aml_task_scope_unref (job->operation);
  job->operation = NULL;
  remove_pack_file (self);

  if (instance == NULL)
    {
      /* The error page only makes sense while the dialog is up; a closed
         dialog that failed in the background just leaves the header. */
      if (!self->closed &&
          !g_error_matches (error, AML_MODPACK_ERROR,
                            AML_MODPACK_ERROR_CANCELLED) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          self->fail_is_install = TRUE;
          push_error (self, "Could not install the modpack", error->message,
                      G_CALLBACK (on_error_retry_install));
        }
      else if (!g_error_matches (error, AML_MODPACK_ERROR,
                                 AML_MODPACK_ERROR_CANCELLED) &&
               !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("the modpack could not be installed: %s", error->message);

      install_job_free (job);
      return;
    }

  /* The instance owns its icon. Never leave its property pointing into the
     shared cache, because cache cleanup is allowed to remove that tree. */
  if (self->icon_path != NULL &&
      g_file_test (self->icon_path, G_FILE_TEST_IS_REGULAR))
    {
      g_autofree char *directory = NULL;
      g_autofree char *instance_icon = NULL;
      g_autoptr (GFile) source = NULL;
      g_autoptr (GFile) target = NULL;
      gboolean icon_copied = FALSE;

      g_object_get (instance, "directory", &directory, NULL);
      instance_icon = g_build_filename (directory, "icon.png", NULL);
      source = g_file_new_for_path (self->icon_path);
      target = g_file_new_for_path (instance_icon);

      if (!aml_fs_copy_file (source, target, G_FILE_COPY_OVERWRITE, NULL,
                             &error))
        g_warning ("the icon of the new instance could not be copied: %s",
                   error != NULL ? error->message : "unknown error");
      else
        {
          icon_copied = aml_instance_set_string (instance, "icon",
                                                 instance_icon, &error);
        }

      if (icon_copied)
        {
          if (!aml_instance_manager_save (self->manager, &error))
            g_warning ("the icon of the new instance could not be saved: %s",
                       error->message);
        }
    }

  install_job_free (job);
}

static void
start_install (AmlDownloadModpackDialog *self)
{
  g_autoptr (GPtrArray) chosen = NULL;
  const char *name = NULL;
  const char *directory = NULL;

  g_return_if_fail (self->modpack != NULL);

  name = self->instance_name;

  if (name == NULL || *name == '\0')
    name = self->modpack->name;

  if (name == NULL || *name == '\0')
    name = self->project != NULL && self->project->title != NULL
           ? self->project->title
           : "modpack";

  directory = self->directory; /* NULL means: the manager's default */

  /* the user's selections for the optional files. A closed dialog has
     none left to read (its switches are gone), so what the pack requires
     is what is installed. */
  chosen = g_ptr_array_new ();

  if (!self->closed && self->opt_switch_rows != NULL)
    {
      for (guint i = 0; i < self->opt_switch_rows->len; i++)
        {
          AdwSwitchRow *row = g_ptr_array_index (self->opt_switch_rows, i);

          if (adw_switch_row_get_active (row))
            {
              AmlModpackFile *file = g_ptr_array_index (self->opt_files, i);

              g_ptr_array_add (chosen, file);
            }
        }
    }

  {
    InstallJob *job = g_new0 (InstallJob, 1);
    AmlDownload *download = self->pack_download;

    /* The same header entry carries on from the compressed pack into the
       extract: it is one operation with two phases. */
    if (download == NULL)
      download = aml_downloads_add (AML_DOWNLOAD_MODPACK, name);

    aml_download_set_detail (
      download, self->modpack->files->len > 0 ? _("Downloading files")
                                              : _("Extracting files"));

    job->dialog = g_object_ref (self);
    job->download = g_object_ref (download);

    job->operation = aml_modpack_install_async (
      self->modpack, self->manager, name, directory, chosen,
      on_install_progress, on_install_done, job);

    /* The × on the header entry is what the user reaches for to stop this,
       so the install follows that entry's cancellable. */
    aml_task_scope_follow_cancellable (job->operation,
                                       aml_download_get_cancellable (download));
  }
}

static GtkWidget *
build_optional_page (AmlDownloadModpackDialog *self,
                     GPtrArray *optional_files)
{
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  AdwPreferencesGroup *heading = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkButton *cancel = GTK_BUTTON (gtk_button_new_with_label (_("Cancel")));
  GtkButton *install = GTK_BUTTON (gtk_button_new_with_label (_("Install")));
  GtkWidget *page = NULL;

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)), GTK_WIDGET (header));

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)),
                                   _("Optional files"));
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (heading),
    _("The pack lists these files but says they are not needed. Nothing of "
      "them is installed unless you ask. Default is off"));

  g_clear_pointer (&self->opt_switch_rows, g_ptr_array_unref);
  self->opt_switch_rows = g_ptr_array_new ();
  g_clear_pointer (&self->opt_files, g_ptr_array_unref);
  self->opt_files = g_ptr_array_new_with_free_func (NULL);

  for (guint i = 0; i < optional_files->len; i++)
    {
      AmlModpackFile *file = g_ptr_array_index (optional_files, i);
      AdwSwitchRow *row = ADW_SWITCH_ROW (adw_switch_row_new ());

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), file->path);
      adw_switch_row_set_active (ADW_SWITCH_ROW (GTK_WIDGET (row)), FALSE);
      gtk_widget_add_css_class (GTK_WIDGET (row), "dim-label");

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)), GTK_WIDGET (row));
      g_ptr_array_add (self->opt_switch_rows, row);
      g_ptr_array_add (self->opt_files, file);
    }

  gtk_widget_add_css_class (GTK_WIDGET (install), "suggested-action");
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (cancel));
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (install));
  gtk_widget_set_margin_start (GTK_WIDGET (buttons), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (buttons), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (buttons), 12);
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_END);

  g_signal_connect (cancel, "clicked", G_CALLBACK (on_optional_cancel),
                    self);
  g_signal_connect (install, "clicked", G_CALLBACK (on_optional_install),
                    self);

  AdwPreferencesPage *page_holder =
    ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  adw_preferences_page_add (page_holder, ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)));

  GtkBox *boxy = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));

  gtk_box_append (GTK_BOX (GTK_WIDGET (boxy)), GTK_WIDGET (page_holder));
  gtk_box_append (GTK_BOX (GTK_WIDGET (boxy)), GTK_WIDGET (buttons));

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)), GTK_WIDGET (boxy));
  page = GTK_WIDGET (adw_navigation_page_new (GTK_WIDGET (toolbar), "Choose Files"));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (GTK_WIDGET (page)), "optional");

  return page;
}

static void
on_optional_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  dialog_details_show_again (self);
}

static void
on_optional_install (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  /* Pop ourselves off so the installing page is showing when
   * the install clocks in with the downloading files. */
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (self->nav));

  start_install (self);
}


/* The name the pack gives itself is what the instance follows. The name
 * field shows the Modrinth project title until the pack is read; a field
 * still holding that title, or holding nothing, is replaced by the pack's
 * own name. A name the user typed is kept exactly as typed. */
static void
adopt_pack_name (AmlDownloadModpackDialog *self)
{
  const char *pack_name = self->modpack != NULL ? self->modpack->name : NULL;
  const char *typed = NULL;

  if (pack_name == NULL || *pack_name == '\0')
    return;

  if (self->name_entry != NULL)
    {
      typed = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));

      if (typed != NULL && *typed != '\0' &&
          g_strcmp0 (typed, self->project != NULL ? self->project->title : NULL)
            != 0)
        return; /* the user named it: that is what it is called */

      gtk_editable_set_text (GTK_EDITABLE (self->name_entry), pack_name);
    }

  if (self->instance_name == NULL || *self->instance_name == '\0' ||
      g_strcmp0 (self->instance_name,
                 self->project != NULL ? self->project->title : NULL) == 0)
    {
      g_free (self->instance_name);
      self->instance_name = g_strdup (pack_name);
    }
}

/* Fetch the .mrpack, parse it, and decide if the user has options. */

/* What the pack file weighs as it arrives, e.g. "312.4/500.0 MB". The
 * manager entry formats it; the dialog only hands the numbers over. */
static void
on_pack_progress (goffset bytes_done, goffset total, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  if (self->pack_download != NULL)
    aml_download_set_bytes (self->pack_download, (guint64) bytes_done,
                            total > 0 ? (guint64) total : 0);
}

static void
on_pack_downloaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_download_file_finish (result, &error))
    {
      if (!g_error_matches (error, AML_DOWNLOAD_ERROR,
                            AML_DOWNLOAD_ERROR_CANCELLED))
        {
          /* A pack that failed after the dialog was closed has nowhere
             to say so; the reason goes to the journal and the entry
             leaves the header. */
          if (!self->closed)
            {
              self->fail_is_install = FALSE;
               push_error (self, _("Pack could not be fetched"), error->message,
                          G_CALLBACK (on_error_retry_fetch));
            }
          else
            g_warning ("the modpack \"%s\" could not be fetched: %s",
                       self->pack_path != NULL ? self->pack_path : "?",
                       error->message);
        }

      if (self->pack_download != NULL)
        aml_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  aml_modpack_new_from_zip_async (
    self->pack_path,
    self->pack_download != NULL
      ? aml_download_get_cancellable (self->pack_download) : self->fetch,
                                  on_pack_parsed, user_data);
}

static void
on_pack_parsed (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;

  g_clear_pointer (&self->modpack, aml_modpack_free);
  self->modpack = aml_modpack_new_from_zip_finish (result, &error);

  if (self->modpack == NULL)
    {
      if (!self->closed)
        {
          self->fail_is_install = FALSE;
           push_error (self, _("Invalid pack"), error->message,
                      G_CALLBACK (on_error_retry_fetch));
        }
      else
        g_warning ("the fetched modpack is not a pack: %s", error->message);

      if (self->pack_download != NULL)
        aml_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  if (!aml_modpack_resolve (self->modpack, &error))
    {
      if (!self->closed)
        {
          self->fail_is_install = FALSE;
           push_error (self, _("The pack does not define its install"),
                      error->message, G_CALLBACK (on_error_retry_fetch));
        }
      else
        g_warning ("the fetched modpack does not define its install: %s",
                   error->message);

      if (self->pack_download != NULL)
        aml_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  /* The pack is known now, so the instance takes its name. A closed
     dialog has no name field left; the name is kept all the same. */
  adopt_pack_name (self);

  {
    g_autoptr (GPtrArray) optional =
      aml_modpack_optional_client_files (self->modpack);

    /* Only a dialog that is still there can ask which optional files to
       take; a pack that arrived after it was closed installs what the
       pack requires. */
    /* A local import is intentionally headless: the dialog is never
     * presented, so there is no optional-file page for the user to answer.
     * Install the required files and overrides directly, leaving optional
     * entries out just as the closed-dialog path does. */
    if (!self->imported && !self->closed && optional->len > 0)
      {
        GtkWidget *page = build_optional_page (self, optional);

        adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                                  ADW_NAVIGATION_PAGE (page));
        operation_done (user_data);
        return;
      }
  }

  /* start_install holds its own reference from here on. Take it before
     this callback gives back the one it holds, so a dialog that is
     already closed is not freed under the install it just started. */
  start_install (self);
  operation_done (user_data);
}

static gboolean
dialog_start_features (AmlDownloadModpackDialog *self)
{
  AmlModrinthFile *file = NULL;

  if (self->version == NULL || self->version->files == NULL)
    return FALSE;

  for (guint i = 0; i < self->version->files->len; i++)
    {
      AmlModrinthFile *candidate = g_ptr_array_index (self->version->files, i);

      if (candidate->filename != NULL &&
          g_str_has_suffix (candidate->filename, ".mrpack"))
        {
          file = candidate;
          break;
        }
    }

  if (file == NULL)
    {
      self->fail_is_install = FALSE;
      push_error (self, "This version has no pack file",
                  "It carries files that AML does not recognize.",
                  G_CALLBACK (on_error_retry_fetch));
      return FALSE;
    }

  /* The pack itself is a download like any other: it goes into the
     header's dropdown and the spinner, and its × cancels it. */
  if (self->pack_download != NULL)
    {
      aml_download_finish (self->pack_download);
      self->pack_download = NULL;
    }

  {
    const char *name = self->instance_name;

    if (name == NULL || *name == '\0')
      name = self->project != NULL ? self->project->title : NULL;

    if (name == NULL || *name == '\0')
      name = "modpack";

    self->pack_download = aml_downloads_add (AML_DOWNLOAD_MODPACK, name);
  }

  aml_download_set_detail (self->pack_download, _("Downloading Modpack"));
  g_object_add_weak_pointer (G_OBJECT (self->pack_download),
                             (gpointer *) &self->pack_download);

  {
    g_autofree char *cache_dir = g_build_filename (
      g_get_user_cache_dir (), "AML", "packs", NULL);
    const char *name = "pack.mrpack";

    aml_fs_make_directory (cache_dir, 0755, NULL);

    if (file->filename != NULL && *file->filename != '\0')
      name = file->filename;

    g_free (self->pack_path);
    self->pack_path = g_build_filename (cache_dir, name, NULL);

    aml_download_file_async (
      file->url, self->pack_path, file->sha1, G_CHECKSUM_SHA1,
      aml_download_get_cancellable (self->pack_download), on_pack_progress,
      on_pack_downloaded, g_object_ref (self));
  }

  return TRUE;
}
