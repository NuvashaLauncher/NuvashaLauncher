/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-content-page-private.h"
#include "nuvasha-icon-load-private.h"
#include "nuvasha-content-index.h"
#include "nuvasha-icons.h"
#include "nuvasha-i18n.h"
#include "nuvasha-modrinth.h"
#include "nuvasha-operation.h"

#define RESULTS_PER_PAGE 20
#define MAX_RESULT_ROWS 400
#define RESULT_ROWS_TRIM 200
#define SEARCH_DELAY_MS 300

/* Internal Browse/search workflow for NuvashaContentPage. */

/* ---------------------------------------------------------- *
 * Browse
 * ---------------------------------------------------------- */

void nuvasha_content_page_on_result_activated (AdwActionRow *row, gpointer user_data);
static void on_category_toggled (GtkCheckButton *button, gpointer user_data);
void nuvasha_content_page_on_scroll_changed (GtkAdjustment *adjustment, gpointer user_data);

/* The list is scrolled to its end: ask for the next page. Only one page
 * is ever on its way, and there is nothing to ask for once the total is
 * reached, so the signal is answered with a flag rather than by
 * counting.
 *
 * GTK4 has no edge-reached signal on GtkScrolledWindow, so the adjustment
 * is watched instead: a scroll to the bottom puts the value at the upper
 * bound. */
void
nuvasha_content_page_on_scroll_changed (GtkAdjustment *adjustment, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  gdouble at_end;

  if (self->browse.model->loading_page)
    return;

  if (self->browse.model->offset >= self->browse.model->total_hits)
    return;

  at_end = gtk_adjustment_get_upper (adjustment) -
           gtk_adjustment_get_page_size (adjustment) -
           gtk_adjustment_get_step_increment (adjustment);

  if (gtk_adjustment_get_value (adjustment) < at_end)
    return;

  nuvasha_content_page_run_search (self, TRUE);
}

/* Retry the failed page using the existing pagination offset. */
void
nuvasha_content_page_on_retry_page_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);

  if (self->browse.model->loading_page)
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->browse.retry_row), FALSE);
  nuvasha_content_page_run_search (self, TRUE);
}

/* The rows of a page of results. @append is FALSE for a new search or a
 * new filter, which starts the list over, and TRUE for the next page,
 * which is added to what is already there. */
/* Puts the project icon in @icon when it has been fetched, and leaves
   the placeholder in it when there is none. The widget is held for the
   length of the fetch, so a row that goes away first is still a widget
   when the answer arrives. */
static void
results_arrived (NuvashaContentPage *self, NuvashaModrinthResults *results,
                 gboolean append)
{
  guint shown = 0;

  if (!append)
    {
      take_rows_out (&self->browse.result_rows,
                     ADW_PREFERENCES_GROUP (self->browse.results_group));
      g_clear_pointer (&self->browse.model->results, g_ptr_array_unref);
      self->browse.model->results = g_ptr_array_new_with_free_func (
          (GDestroyNotify) nuvasha_modrinth_project_free);
      self->browse.model->total_hits = results->total_hits;
      g_hash_table_remove_all (self->browse.model->seen_ids);

      /* A new search starts at the top of the list. */
      {
        GtkAdjustment *adjustment =
          gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (
            self->browse.results_scroller));

        gtk_adjustment_set_value (adjustment, gtk_adjustment_get_lower (adjustment));
      }
    }

  for (guint i = 0; i < results->projects->len; i++)
    {
      NuvashaModrinthProject *project = g_ptr_array_index (results->projects, i);
      GtkWidget *row = NULL;
      GtkWidget *icon = NULL;

      /* A project already listed is not listed twice: the same answer can
       * come back for two pages that overlap. */
      if (g_hash_table_contains (self->browse.model->seen_ids, project->project_id))
        continue;

      {
        g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
          "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-content-result-row.ui");
        row = GTK_WIDGET (g_object_ref (gtk_builder_get_object (builder, "row")));
      }
      icon = nuvasha_icon_widget_new (nuvasha_content_type_icon (self->type));

      g_hash_table_add (self->browse.model->seen_ids, g_strdup (project->project_id));

      /* Cache project metadata for installed rows. */
      if (project->icon_url != NULL && *project->icon_url != '\0')
        g_hash_table_insert (self->browse.model->project_icons,
                             g_strdup (project->project_id),
                             g_strdup (project->icon_url));

      if (project->description != NULL && *project->description != '\0')
        g_hash_table_insert (self->browse.model->project_descriptions,
                             g_strdup (project->project_id),
                             g_strdup (project->description));

      if (project->title != NULL && *project->title != '\0')
        g_hash_table_insert (self->browse.model->project_titles,
                             g_strdup (project->project_id),
                             g_strdup (project->title));

      /* Show the icon, title, and short description. Details contain the
         author, downloads, game version, and loader. */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                     project->title);
      {
        g_autofree char *line = description_line (project->description, 0);

        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), line);
      }
      /* Limit row descriptions to two lines; the full text is on the details
         page. */
      adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
      adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), FALSE);

      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      nuvasha_icon_load_start (icon, project->icon_url);

      /* The rows point into the list the page owns, so the projects
       * live exactly as long as the rows that show them. */
      g_ptr_array_add (self->browse.model->results, nuvasha_modrinth_project_copy (project));
      g_object_set_data (G_OBJECT (row), "nuvasha-project",
                         g_ptr_array_index (self->browse.model->results,
                                            self->browse.model->results->len - 1));

      adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);

      /* The whole row opens the project, where the version is chosen.
       * An AdwActionRow is not activatable by default, so the row
       * itself is made one: a click on it activates the row. */
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
       g_signal_connect_object (row, "activated", G_CALLBACK (nuvasha_content_page_on_result_activated),
                                self, 0);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->browse.results_group),
                                 row);
      self->browse.result_rows = g_list_append (self->browse.result_rows, row);
      shown++;
    }

  /* The offset is where the next page starts, which is the end of what
     is listed now. */
  self->browse.model->offset += shown;

  /* The list does not grow without end: past the cap the oldest rows go,
     and the scroll is put back where it was, so the user stays on the
     same project rather than being thrown to the top. */
  if (self->browse.result_rows != NULL &&
      g_list_length (self->browse.result_rows) > MAX_RESULT_ROWS)
    {
      GtkAdjustment *adjustment =
        gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (
          self->browse.results_scroller));
      gdouble at = gtk_adjustment_get_value (adjustment);
      gdouble removed = 0;

      for (guint i = 0; i < RESULT_ROWS_TRIM; i++)
        {
          GList *oldest = self->browse.result_rows;
          GtkWidget *row = GTK_WIDGET (oldest->data);
          gint height = gtk_widget_get_height (GTK_WIDGET (row));

          removed += height > 0 ? height : 48;

          self->browse.result_rows = g_list_delete_link (self->browse.result_rows, oldest);
          adw_preferences_group_remove (ADW_PREFERENCES_GROUP (
                                          self->browse.results_group),
                                        row);
        }

      gtk_adjustment_set_value (adjustment,
                                MAX (at - removed,
                                     gtk_adjustment_get_lower (adjustment)));
    }

  {
    gboolean empty = self->browse.model->results->len == 0;

    gtk_widget_set_visible (GTK_WIDGET (self->browse.results_status), empty);
    gtk_widget_set_visible (GTK_WIDGET (self->browse.results_scroller), !empty);
    gtk_widget_set_visible (GTK_WIDGET (self->browse.results_group), TRUE);
    gtk_widget_set_visible (GTK_WIDGET (self->browse.loading_more), FALSE);
    gtk_widget_set_visible (GTK_WIDGET (self->browse.retry_row), FALSE);

    if (self->browse.model->offset >= self->browse.model->total_hits)
      gtk_widget_set_visible (GTK_WIDGET (self->browse.loading_more), FALSE);
  }
}

static void
search_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
              gpointer user_data)
{
  PageWatch *watch = user_data;
  NuvashaContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModrinthResults) results = NULL;
  gboolean append = FALSE;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  append = self->browse.model->pending_append;
  results = nuvasha_modrinth_search_finish (result, &error);
  set_busy (self, FALSE);
  self->browse.model->loading_page = FALSE;

  if (results == NULL)
    {
      take_rows_out (&self->browse.result_rows,
                 ADW_PREFERENCES_GROUP (self->browse.results_group));
      g_clear_pointer (&self->browse.model->results, g_ptr_array_unref);
      gtk_widget_set_visible (GTK_WIDGET (self->browse.results_status), TRUE);
      gtk_widget_set_visible (GTK_WIDGET (self->browse.results_group), FALSE);
      gtk_widget_set_visible (GTK_WIDGET (self->browse.loading_more), FALSE);

      /* A page that failed is offered again from the list itself. */
      if (append)
        {
          g_autofree char *message =
            error != NULL ? g_strdup (error->message)
                   : g_strdup (_("the next page could not be loaded"));

          gtk_label_set_text (GTK_LABEL (self->browse.retry_label), message);
          gtk_widget_set_visible (GTK_WIDGET (self->browse.retry_row), TRUE);
        }
      else if (error != NULL &&
               !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_autofree char *escaped = g_markup_escape_text (error->message, -1);

          adw_status_page_set_description (ADW_STATUS_PAGE (self->browse.results_status),
                                           escaped);
        }

      page_watch_free (watch);
      return;
    }

  results_arrived (self, results, append);
  page_watch_free (watch);
}

void
nuvasha_content_page_run_search (NuvashaContentPage *self, gboolean append)
{
  g_autoptr (NuvashaModrinthSearch) search = g_new0 (NuvashaModrinthSearch, 1);

  search->type = self->type;
  search->query = g_strdup (self->browse.model->query != NULL ? self->browse.model->query : "");
  search->minecraft_version = g_strdup (self->minecraft_version);
  search->loader = self->loader != NULL ? g_strdup (self->loader) : NULL;
  search->categories = self->browse.model->categories != NULL
                           ? g_ptr_array_ref (self->browse.model->categories)
                           : NULL;
  search->offset = self->browse.model->offset;
  search->limit = RESULTS_PER_PAGE;

  /* A new search or a new filter cancels the page that was on its way
   * and starts the list over. */
  if (!append)
    {
      g_cancellable_cancel (self->browse.model->more);
      g_clear_object (&self->browse.model->more);
      self->browse.model->more = g_cancellable_new ();
      self->browse.model->offset = 0;
    }

  self->browse.model->loading_page = TRUE;
  self->browse.model->pending_append = append;
  set_busy (self, TRUE);

  /* Show a spinner while loading the next page. */
  gtk_widget_set_visible (GTK_WIDGET (self->browse.loading_more), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->browse.retry_row), FALSE);

  {
    PageWatch *watch = page_watch_new (self);

    watch->operation = nuvasha_modrinth_search_async (search, search_ready, watch);
    nuvasha_operation_follow (watch->operation,
                                       self->browse.model->more);
  }
}

static gboolean
on_search_timeout (gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);

  nuvasha_operation_forget_source (self->browse.model->scope,
                                self->browse.model->debounce_source);
  self->browse.model->debounce_source = 0;
  g_free (self->browse.model->query);
  self->browse.model->query =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->browse.search_entry)));
  self->browse.model->offset = 0;
  nuvasha_content_page_run_search (self, FALSE);

  return G_SOURCE_REMOVE;
}

void
nuvasha_content_page_on_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);

  if (self->browse.model->debounce_source != 0)
    nuvasha_operation_remove (self->browse.model->scope,
                                  self->browse.model->debounce_source);

  self->browse.model->debounce_source =
    g_timeout_add (SEARCH_DELAY_MS, on_search_timeout, self);
  g_source_set_name_by_id (self->browse.model->debounce_source,
                           "nuvasha-content-search-debounce");
  nuvasha_operation_track (self->browse.model->scope,
                               self->browse.model->debounce_source);
}

/* Escape clears the search field. The search bar is always visible, so
 * Escape is not used to close it.
 *
 * GTK4 has no key-press-event signal and no stop-searching signal on
 * GtkSearchEntry, so the key is taken with a controller of its own. It is
 * added after the one inside the search entry and both run on the bubble
 * phase, so this one is asked first.
 *
 * Clearing the text emits "changed" and reloads the unfiltered results. */
gboolean
nuvasha_content_page_on_search_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
               guint keyval, guint keycode G_GNUC_UNUSED,
               GdkModifierType state G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);

  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;

  if (*gtk_editable_get_text (
        GTK_EDITABLE (self->browse.search_entry)) != '\0')
    gtk_editable_set_text (GTK_EDITABLE (self->browse.search_entry), "");

  return GDK_EVENT_STOP;
}

static GtkWidget *
active_search_entry (NuvashaContentPage *self)
{
  const char *visible = self->stack != NULL
    ? adw_view_stack_get_visible_child_name (ADW_VIEW_STACK (self->stack))
    : NULL;

  return g_strcmp0 (visible, "browse") == 0
           ? GTK_WIDGET (self->browse.search_entry)
           : GTK_WIDGET (self->installed.search_entry);
}

gboolean
nuvasha_content_page_on_content_search_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
                       guint keyval, guint keycode G_GNUC_UNUSED,
                       GdkModifierType state, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  GtkWidget *entry;

  if (keyval != GDK_KEY_f || !(state & GDK_CONTROL_MASK))
    return GDK_EVENT_PROPAGATE;

  entry = active_search_entry (self);
  if (entry != NULL)
    {
      gtk_widget_set_visible (entry, TRUE);
      gtk_widget_grab_focus (entry);
    }

  return GDK_EVENT_STOP;
}

static void categories_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void fill_categories (NuvashaContentPage *self, GPtrArray *categories);

static void
load_categories (NuvashaContentPage *self)
{
  g_autofree char *project_type =
    nuvasha_modrinth_project_type_for_content (self->type);

  self->browse.model->categories_loaded = TRUE;
  {
    PageWatch *watch = page_watch_new (self);

    watch->operation = nuvasha_modrinth_categories_async (project_type,
                                                      categories_ready, watch);
    nuvasha_operation_follow (watch->operation,
                                       self->browse.model->cancellable);
  }
}

static void
load_categories_again (GtkWidget *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);

  self->browse.model->categories_loaded = FALSE;
  load_categories (self);
}

/* Fills the popover with the categories, grouped under the heading
 * Modrinth puts them under, with readable labels. */
static void
fill_categories (NuvashaContentPage *self, GPtrArray *categories)
{
  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->browse.filter_box));

  while (child != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      gtk_box_remove (GTK_BOX (self->browse.filter_box), child);
      child = next;
    }

  {
    g_autofree char *current = NULL;

    for (guint i = 0; i < categories->len; i++)
      {
        NuvashaModrinthCategory *category = g_ptr_array_index (categories, i);
        GtkWidget *button = NULL;

        if (g_strcmp0 (current, category->header) != 0)
          {
            g_autofree char *heading_label =
              category->header != NULL
                  ? nuvasha_modrinth_category_label (category->header)
                   : g_strdup (_("Other"));
            GtkLabel *heading = GTK_LABEL (gtk_label_new (heading_label));

            g_free (current);
            current = g_strdup (category->header != NULL ? category->header
                                                        : "other");

            gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
            gtk_widget_add_css_class (GTK_WIDGET (heading), "caption");
            gtk_widget_add_css_class (GTK_WIDGET (heading), "dim-label");
            gtk_widget_set_margin_top (GTK_WIDGET (heading), 6);
            gtk_widget_set_margin_start (GTK_WIDGET (heading), 6);
            gtk_box_append (GTK_BOX (self->browse.filter_box), GTK_WIDGET (heading));
          }

        {
          g_autofree char *label_text =
            nuvasha_modrinth_category_label (category->name);

          button = gtk_check_button_new_with_label (label_text);
        }

        gtk_widget_add_css_class (GTK_WIDGET (button), "flat");

        gtk_widget_set_hexpand (GTK_WIDGET (button), TRUE);
        gtk_widget_set_halign (GTK_WIDGET (button), GTK_ALIGN_FILL);
        g_object_set_data_full (G_OBJECT (button), "nuvasha-category",
                                g_strdup (category->name), g_free);
        g_signal_connect_object (button, "toggled", G_CALLBACK (on_category_toggled),
                                 self, 0);
        gtk_box_append (GTK_BOX (self->browse.filter_box), GTK_WIDGET (button));
      }
  }
}

static void
categories_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  PageWatch *watch = user_data;
  NuvashaContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) categories = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  categories = nuvasha_modrinth_categories_finish (result, &error);

  if (self->browse.filter_box == NULL)
    {
      page_watch_free (watch);
      return;
    }

  /* A failed request is said so, with a way to try again: an empty list
   * of check buttons reads as "no categories", not as a failure. */
  if (categories == NULL)
    {
      GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->browse.filter_box));

      while (child != NULL)
        {
          GtkWidget *next = gtk_widget_get_next_sibling (child);

          gtk_box_remove (GTK_BOX (self->browse.filter_box), child);
          child = next;
        }

      {
        GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 6));
        GtkLabel *label = GTK_LABEL (gtk_label_new (
            error != NULL ? error->message : _("Could not load categories")));
        GtkButton *retry = GTK_BUTTON (gtk_button_new_with_label (_("Retry")));

        gtk_label_set_wrap (GTK_LABEL (label), TRUE);
        gtk_widget_set_size_request (GTK_WIDGET (box), 220, -1);
        gtk_box_append (GTK_BOX (box), GTK_WIDGET (label));
        gtk_box_append (GTK_BOX (box), GTK_WIDGET (retry));

        gtk_box_append (GTK_BOX (self->browse.filter_box), GTK_WIDGET (box));
        g_signal_connect_object (retry, "clicked",
                                 G_CALLBACK (load_categories_again), self, 0);
      }

      page_watch_free (watch);
      return;
    }

  fill_categories (self, categories);
  page_watch_free (watch);
}

void
nuvasha_content_page_on_filter_clicked (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);

  if (self->browse.model->categories_loaded)
    return;

  load_categories (self);
}

static void
on_category_toggled (GtkCheckButton *button, gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  const char *category =
    g_object_get_data (G_OBJECT (button), "nuvasha-category");

  if (category == NULL)
    return;

  if (gtk_check_button_get_active (button))
    g_ptr_array_add (self->browse.model->categories, g_strdup (category));
  else
    {

      GPtrArray *kept = g_ptr_array_new_with_free_func (g_free);

      for (guint i = 0; i < self->browse.model->categories->len; i++)
        {
          const char *one = g_ptr_array_index (self->browse.model->categories, i);

          if (g_strcmp0 (one, category) != 0)
            g_ptr_array_add (kept, g_strdup (one));
        }

      g_clear_pointer (&self->browse.model->categories, g_ptr_array_unref);
      self->browse.model->categories = kept;
    }

  self->browse.model->offset = 0;
  nuvasha_content_page_run_search (self, FALSE);
}
