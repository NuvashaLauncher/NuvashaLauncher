/* Modrinth modpack browsing and search. */
#include "nuvashalauncher-download-modpack-dialog-private.h"
#include "nuvashalauncher-icon-load-private.h"
#include "nuvashalauncher-content-format.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-operation.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-markdown.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-task.h"
#include <json-glib/json-glib.h>
#include <string.h>

#define RESULTS_PER_PAGE 20
#define SEARCH_DELAY_MS 500

static void run_search (nuvashalauncherDownloadModpackDialog *self, gboolean append);
static void on_search_done (GObject *, GAsyncResult *, gpointer);

/* ---------------------------------------------------------- *
 * The browser page
 * ---------------------------------------------------------- */

static gboolean
on_search_timeout (gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);

  nuvashalauncher_operation_forget_source (self->scope, self->debounce);
  self->debounce = 0;
  run_search (self, FALSE);

  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);

  if (self->debounce != 0)
    nuvashalauncher_operation_remove (self->scope, self->debounce);

  self->debounce = g_timeout_add (SEARCH_DELAY_MS, on_search_timeout, self);
  g_source_set_name_by_id (self->debounce, "nuvashalauncher-modpack-search-debounce");
  nuvashalauncher_operation_track (self->scope, self->debounce);
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
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);
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

/* Retry a failed results page. */
static void
on_retry_page (nuvashalauncherDownloadModpackDialog *self)
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

/* Open the selected pack, or close its details view if it is already open. */
static void
result_row_activated (AdwActionRow *row, gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);
  nuvashalauncherModrinthProject *project =
    g_object_get_data (G_OBJECT (row), "nuvashalauncher-project");

  if (project == NULL)
    return;

  if (self->details_page != NULL && self->project != NULL &&
      g_strcmp0 (self->project->project_id, project->project_id) == 0)
    {
      close_details (self);
      return;
    }

  g_clear_pointer (&self->project, nuvashalauncher_modrinth_project_free);
  g_clear_pointer (&self->version, nuvashalauncher_modrinth_version_free);
  self->project = nuvashalauncher_modrinth_project_copy (project);

  dialog_details_open (self);
}

/* State retained while loading one row icon. */
/* Kicks off one row's icon and remembers the url on the widget, so a
 * failed decode can name it in the log. */
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
results_arrived (nuvashalauncherDownloadModpackDialog *self, nuvashalauncherModrinthResults *results,
                 gboolean append)
{
  guint shown = 0;

  if (!append)
    {
      take_rows_out (&self->rows, ADW_PREFERENCES_GROUP (self->results_group));
      g_clear_pointer (&self->hits, g_ptr_array_unref);
      self->hits = g_ptr_array_new_with_free_func (
          (GDestroyNotify) nuvashalauncher_modrinth_project_free);
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
          nuvashalauncherModrinthProject *project =
              g_ptr_array_index (results->projects, i);
          GtkWidget *row = NULL;
          GtkWidget *icon = NULL;

          /* The same project is never listed twice, however the pages
             overlap. */
          if (g_hash_table_contains (self->seen_ids, project->project_id))
            continue;

          row = adw_action_row_new ();
          icon = nuvashalauncher_icon_widget_new ("view-list-symbolic");

          g_hash_table_add (self->seen_ids, g_strdup (project->project_id));

          /* The icon, then the name and the description under it: the
             author, the download count, the game version and the loader
             are what the details page is for. Markup off first: the
             title and the description are plain Modrinth text and may
             hold an "&". */
          adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row),
                                              FALSE);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                         project->title);
          {
            g_autofree char *line = one_line (project->description);

            adw_action_row_set_subtitle (ADW_ACTION_ROW (row), line);
          }
          /* Two lines at most, ending in an ellipsis when the
             description is longer: every row is the same height, and the
             whole description is on the details page. */
          adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
          adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), FALSE);

          gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
          nuvashalauncher_icon_load_start (icon, project->icon_url);

          adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);

          /* An AdwActionRow is not activatable by default, so the row
             itself is made one: a click anywhere on it opens the
             details page. */
          gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

          g_ptr_array_add (self->hits,
                           nuvashalauncher_modrinth_project_copy (project));
          g_object_set_data (G_OBJECT (row), "nuvashalauncher-project",
                             g_ptr_array_index (self->hits,
                                                self->hits->len - 1));
          g_ptr_array_add (self->rows, row);

          g_signal_connect_object (row, "activated",
                                   G_CALLBACK (result_row_activated), self, 0);
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

      adw_view_stack_set_visible_child_name (
          self->results_stack, empty ? "empty" : "results");
    }

  gtk_widget_set_visible (GTK_WIDGET (self->loading_more), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry_row), FALSE);

  self->loading_page = FALSE;
}

static void
run_search (nuvashalauncherDownloadModpackDialog *self, gboolean append)
{
  const char *query =
    gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  g_autofree char *escaped_query = NULL;
  g_autofree char *escaped_facets = NULL;
  g_autofree char *url = NULL;
  typedef struct {
    nuvashalauncherDownloadModpackDialog *dialog;
    guint generation;
  } SearchAsk;
  SearchAsk *ask = NULL;

  if (self->search != NULL)
    g_cancellable_cancel (self->search);

  g_clear_object (&self->search);
  self->search = g_cancellable_new ();

  if (self->debounce != 0)
    {
      /* The debounce source belongs to the dialog scope. Removing it
         through the scope also forgets its ID, so a later recycled GLib
         source ID cannot be removed during scope teardown. */
      nuvashalauncher_operation_remove (self->scope, self->debounce);
      self->debounce = 0;
    }

  if (!append)
    self->offset = 0;

  self->loading_page = TRUE;
  self->append = append;
  ask = g_new0 (SearchAsk, 1);
  ask->dialog = g_object_ref (self);
  ask->generation = ++self->search_generation;

  /* Show the full loading state for the first page and an inline spinner for
     subsequent pages. */
  if (!append)
    adw_view_stack_set_visible_child_name (self->results_stack,
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

  /* Empty searches show popular packs ordered by downloads. */
  if (query == NULL || *query == '\0')
    url = g_strdup_printf (
      nuvashalauncher_MODRINTH_URL "/search?query=%s&facets=%s&index=downloads"
                       "&offset=%u&limit=%u",
      escaped_query, escaped_facets, self->offset, RESULTS_PER_PAGE);
  else
    url = g_strdup_printf (
      nuvashalauncher_MODRINTH_URL "/search?query=%s&facets=%s&offset=%u&limit=%u",
      escaped_query, escaped_facets, self->offset, RESULTS_PER_PAGE);

  /* The callback holds a reference, so closing the dialog while a search
   * is in flight finalizes it, and a callback that then reads self would
   * be reading freed memory. */
  nuvashalauncher_operation_release (
    nuvashalauncher_download_json_async (nuvashalauncher_downloads_get_service (), url, on_search_done, ask),
    self->search);
}

/* Every operation of this dialog hands its callback a reference it gives
   back here: the dialog may be closed, and finalized, while the work is
   still going on. */
void
operation_done (gpointer user_data)
{
  g_object_unref (user_data);
}

static void
on_search_done (GObject *source G_GNUC_UNUSED,
                GAsyncResult *result,
                gpointer user_data)
{
  typedef struct {
    nuvashalauncherDownloadModpackDialog *dialog;
    guint generation;
  } SearchAsk;
  SearchAsk *ask = user_data;
  nuvashalauncherDownloadModpackDialog *self = ask->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (nuvashalauncherModrinthResults) results = NULL;
  const char *answer = NULL;

  parser = nuvashalauncher_download_json_finish (result, &error);

  if (ask->generation != self->search_generation)
    {
      g_object_unref (self);
      g_free (ask);
      return;
    }

  if (parser == NULL)
    {
      if (!g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                            nuvashalauncher_DOWNLOAD_ERROR_CANCELLED))
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
                 _("Could not search Modrinth"));
              adw_status_page_set_description (
                ADW_STATUS_PAGE (self->error_status), escaped);
              adw_view_stack_set_visible_child_name (self->results_stack,
                                                     "error");
            }
        }

      g_object_unref (self);
      g_free (ask);
      return;
    }

  answer = nuvashalauncher_modrinth_json_node_to_string (json_parser_get_root (parser));
  results = nuvashalauncher_modrinth_results_from_json (answer, -1, &error);

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
                                      _("Could not understand the answer"));
          adw_status_page_set_description (
            ADW_STATUS_PAGE (self->error_status), escaped);
          adw_view_stack_set_visible_child_name (self->results_stack,
                                                 "error");
        }

      g_object_unref (self);
      g_free (ask);
      return;
    }

  results_arrived (self, results, self->append);
  g_object_unref (self);
  g_free (ask);
}

void
build_browse_page (nuvashalauncherDownloadModpackDialog *self, gboolean start_search)
{
  GtkWidget *page = NULL;
  GtkBox *root = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  g_autoptr (GtkBuilder) toolbar_builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-browser-toolbar.ui");
  GtkBox *top = GTK_BOX (gtk_builder_get_object (toolbar_builder, "top"));
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_autoptr (GtkBuilder) states_builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-browser-states.ui");
  GtkBox *loading = GTK_BOX (gtk_builder_get_object (states_builder, "loading"));
  GtkBox *loading_more = GTK_BOX (gtk_builder_get_object (states_builder, "loading_more"));
  GtkBox *retry_row = GTK_BOX (gtk_builder_get_object (states_builder, "retry_row"));
  GtkLabel *retry_label = GTK_LABEL (gtk_builder_get_object (states_builder, "retry_label"));
  GtkButton *retry_button = GTK_BUTTON (gtk_builder_get_object (states_builder, "retry_button"));

  /* Exactly one of these is shown at any moment: the list, the words for
     an empty answer, the error, or the spinner while the first page is
     on its way. A status page that stayed under the results was the bug
     this replaces. */
  self->results_stack = ADW_VIEW_STACK (adw_view_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->results_stack), TRUE);

  self->results_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->results_group),
                            "nuvashalauncher-modpack-results");

  /* The list starts where the search row above it ends: its first row is
     not rounded at the top, so the two read as one piece. */

  self->search_entry = GTK_SEARCH_ENTRY (gtk_builder_get_object (
    toolbar_builder, "search-entry"));
  g_signal_connect_object (self->search_entry, "changed",
                           G_CALLBACK (on_search_changed), self, 0);

  gtk_box_append (GTK_BOX (root), GTK_WIDGET (top));

  /* The list. The bottom of it holds a spinner while the next page is on
     its way and a retry row when a page failed. */
  gtk_widget_set_visible (GTK_WIDGET (loading_more), FALSE);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->results_group),
                              GTK_WIDGET (loading_more));

  gtk_widget_add_css_class (GTK_WIDGET (retry_label), "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->results_group),
                              GTK_WIDGET (retry_row));

  self->loading_more = GTK_BOX (loading_more);
  self->retry_row = GTK_BOX (retry_row);
  self->retry_label = GTK_LABEL (retry_label);

  g_signal_connect_swapped (retry_button, "clicked",
                            G_CALLBACK (on_retry_page), self);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  /* The group is the whole width of the list: a row of the browser has to
     cover it rather than sit centered in a clamp of its own. */
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 GTK_WIDGET (self->results_group));
  gtk_widget_set_vexpand (GTK_WIDGET (scroller), TRUE);
  adw_view_stack_add_named (self->results_stack, GTK_WIDGET (scroller), "results");

  /* An empty answer. */
  self->results_status = ADW_STATUS_PAGE (g_object_ref (
    gtk_builder_get_object (states_builder, "empty")));
  gtk_widget_add_css_class (GTK_WIDGET (self->results_status), "nuvashalauncher-status-page");
  adw_view_stack_add_named (self->results_stack,
                       GTK_WIDGET (self->results_status), "empty");

  /* A search that could not be made. */
  self->error_status = ADW_STATUS_PAGE (g_object_ref (
    gtk_builder_get_object (states_builder, "error")));
  gtk_widget_add_css_class (GTK_WIDGET (self->error_status), "nuvashalauncher-status-page");
  adw_view_stack_add_named (self->results_stack,
                       GTK_WIDGET (self->error_status), "error");

  /* The first page is on its way. */
  adw_view_stack_add_named (self->results_stack, GTK_WIDGET (loading), "loading");

  adw_view_stack_set_visible_child_name (self->results_stack,
                                    "loading");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->results_stack));

  /* The list asks for the next page when it is scrolled to its end.
     GTK4 has no edge-reached signal, so the adjustment is watched. */
  {
    GtkAdjustment *adjustment =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));

    g_signal_connect_object (adjustment, "value-changed",
                             G_CALLBACK (on_scroll_changed), self, 0);
  }

  /* The list is the left half; the pack that is opened is the right half,
     empty until one is opened. Collapsed shows one page at a time, which
     is the list alone. */
  self->split = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->split), "background");
  gtk_widget_set_vexpand (GTK_WIDGET (self->split), TRUE);
   /* Named so the navigation split view can identify the browse page. */
   {
     AdwNavigationPage *sidebar = ADW_NAVIGATION_PAGE (
       adw_navigation_page_new (GTK_WIDGET (box), _("Browse Modpacks")));

     gtk_widget_add_css_class (GTK_WIDGET (sidebar), "background");
     adw_navigation_split_view_set_sidebar (
       ADW_NAVIGATION_SPLIT_VIEW (self->split), sidebar);
   }
   adw_navigation_split_view_set_collapsed (
     ADW_NAVIGATION_SPLIT_VIEW (self->split),
     gtk_widget_get_width (GTK_WIDGET (self)) < 820);

  adw_navigation_split_view_set_sidebar_width_fraction (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), 0.5);
   adw_navigation_split_view_set_min_sidebar_width (
     ADW_NAVIGATION_SPLIT_VIEW (self->split), 360);
   adw_navigation_split_view_set_max_sidebar_width (
     ADW_NAVIGATION_SPLIT_VIEW (self->split), 640);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (self->split));

  page = GTK_WIDGET (adw_navigation_page_new (GTK_WIDGET (root), ""));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (page), "browse");

  adw_navigation_view_add (ADW_NAVIGATION_VIEW (self->nav),
                           ADW_NAVIGATION_PAGE (page));

  /* The page lands on the popular packs rather than on nothing. Local
   * imports keep the shared navigation structure but do not request an
   * unrelated list of popular packs. */
  if (start_search)
    run_search (self, FALSE);
}
