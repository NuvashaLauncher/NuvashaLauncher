/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlDownloadModpackDialog: browsing Modrinth for modpacks and putting
 * one into a new instance.
 *
 * The flow is a navigation view with a Browse page, a Details page and
 * an Installing page, plus an error page the flow falls onto and a
 * page with the pack's optional files when it lists any. Nothing the
 * pack needs for its Minecraft version or its loader is asked of the
 * user: both come from what the pack says about itself. */

#include "aml-download-modpack-dialog.h"

#include "aml-download.h"
#include "aml-modpack.h"
#include "aml-modrinth.h"

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

  GtkWidget *nav; /* AdwNavigationView */

  /* The cache of one downloaded pack, reused while the dialog lives. */
  char *pack_path;

  /* What the user did by the end of the details page. */
  AmlModrinthProject *project;
  AmlModrinthVersion *version;

  AmlModpack *modpack; /* owned, parsed from pack_path */

  /* Browse page widgets. */
  GtkWidget *search_entry;
  GtkWidget *results_status;
  GtkWidget *results_group;
  GtkWidget *spinner;
  GtkWidget *previous;
  GtkWidget *next;
  GtkWidget *page_label;
  guint offset;
  guint64 total;

  /* The details of a pack that comes and goes: versions of it. */
  GPtrArray *versions; /* AmlModrinthVersion* of the details page */
  GtkWidget *version_row; /* AdwComboRow */
  GtkWidget *name_entry;  /* AdwEntryRow */
  GtkWidget *directory_row;
  GtkWidget *install_button;

  /* Widgets of the pages that fall on top of the stack. */
  GtkWidget *install_bar;
  GtkWidget *install_count;
  GtkWidget *install_cancel;

  /* What was being retried when an error was shown. */
  gboolean fail_is_install;

  /* The switches of the optional-files page and the files they word;
   * for the question "which of the optional files does the user wish?"
   * Both are reset each visit. */
  GPtrArray *opt_switch_rows; /* AdwSwitchRow*/
  GPtrArray *opt_files;       /* borrowed/self counterpart list */


  /* The running parts. */
  GCancellable *search; /* kills an in-flight search */
  GCancellable *fetch;  /* kills a version fetch, the pack get and install */
  guint debounce;
  GPtrArray *rows;
  GPtrArray *hits;

  char *directory;     /* where the instance goes, NULL for the default */
  char *instance_name; /* what the user typed on the details page */
};

G_DEFINE_FINAL_TYPE (AmlDownloadModpackDialog, aml_download_modpack_dialog,
                     ADW_TYPE_DIALOG)

static void run_search (AmlDownloadModpackDialog *self);
static void set_busy (AmlDownloadModpackDialog *self, gboolean busy);
static void on_search_done (GObject *, GAsyncResult *, gpointer);
static void build_browse_page (AmlDownloadModpackDialog *self);
static GtkWidget * build_optional_page (AmlDownloadModpackDialog *self,
                                        GPtrArray *optional_files);
static GtkWidget * build_error_page (AmlDownloadModpackDialog *self,
                                     const char *what,
                                     const char *message,
                                     GCallback retry,
                                     GCallback cancel);
static GtkWidget * build_installing_page (AmlDownloadModpackDialog *self);
static void start_install (AmlDownloadModpackDialog *self);
static void on_pick_directory (GtkButton *, gpointer);
static void on_cancel_clicked (GtkButton *, gpointer);
static void on_install_clicked (GtkButton *, gpointer);
static void on_error_back (GtkButton *, gpointer);
static void on_error_retry_fetch (GtkButton *, gpointer);
static void on_error_retry_install (GtkButton *, gpointer);
static void on_install_cancel (GtkButton *, gpointer);
static void on_optional_cancel (GtkButton *, gpointer);
static void on_optional_install (GtkButton *, gpointer);
static void dialog_details_open (AmlDownloadModpackDialog *self);
static void dialog_start_features (AmlDownloadModpackDialog *self);

static void
dialog_dispose (GObject *object)
{
  AmlDownloadModpackDialog *self = AML_DOWNLOAD_MODPACK_DIALOG (object);

  if (self->search != NULL)
    g_cancellable_cancel (self->search);

  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);

  if (self->debounce != 0)
    g_source_remove (self->debounce);

  g_clear_object (&self->manager);
  g_clear_object (&self->search);
  g_clear_object (&self->fetch);
  g_clear_pointer (&self->project, aml_modrinth_project_free);
  g_clear_pointer (&self->version, aml_modrinth_version_free);
  g_clear_pointer (&self->modpack, aml_modpack_free);
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  g_clear_pointer (&self->rows, g_ptr_array_unref);
  g_clear_pointer (&self->hits, g_ptr_array_unref);
  g_clear_pointer (&self->opt_switch_rows, g_ptr_array_unref);
  g_clear_pointer (&self->opt_files, g_ptr_array_unref);
  g_free (self->pack_path);
  g_free (self->directory);
  g_free (self->instance_name);
  self->instance_name = NULL;

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
aml_download_modpack_dialog_init (AmlDownloadModpackDialog *self)
{
  self->search = g_cancellable_new ();
  self->fetch = g_cancellable_new ();
  self->rows = NULL;
  self->hits = NULL;
  self->versions = NULL;

  adw_dialog_set_title (ADW_DIALOG (self), "Download Modpack");
  adw_dialog_set_content_width (ADW_DIALOG (self), 720);
  adw_dialog_set_content_height (ADW_DIALOG (self), 560);

  self->nav = adw_navigation_view_new ();
  adw_dialog_set_child (ADW_DIALOG (self), self->nav);

  build_browse_page (self);
}

AmlDownloadModpackDialog *
aml_download_modpack_dialog_new (AmlInstanceManager *manager)
{
  AmlDownloadModpackDialog *self =
    g_object_new (AML_TYPE_DOWNLOAD_MODPACK_DIALOG, NULL);

  self->manager = g_object_ref (manager);

  return self;
}

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

/* Minecraft version names ranked the way a user thinks: by part, the
 * way "1.21.2" sits above "1.9.4". */
static gint
mc_version_cmp (const char *a, const char *b)
{
  g_auto (GStrv) pa = g_strsplit (a, ".", -1);
  g_auto (GStrv) pb = g_strsplit (b, ".", -1);

  for (guint i = 0; ; i++)
    {
      const char *sa = pa[i];
      const char *sb = pb[i];

      if (sa == NULL && sb == NULL)
        return 0;

      if (sa == NULL)
        return -1;

      if (sb == NULL)
        return 1;

      {
        glong an = strtol (sa, NULL, 10);
        glong bn = strtol (sb, NULL, 10);

        if (an != bn)
          return an < bn ? -1 : 1;
      }
    }
}

static const char *
project_loader_name (AmlModrinthProject *project)
{
  /* Modrinth tags a project with its loaders; the first of the ones
   * AML knows is the one the summary shows, falling back to vanilla. */
  static const char *known[] = { "fabric", "forge", "neoforge", "quilt" };

  if (project->categories != NULL)
    {
      for (guint i = 0; i < project->categories->len; i++)
        {
          const char *tag = g_ptr_array_index (project->categories, i);

          for (guint j = 0; j < G_N_ELEMENTS (known); j++)
            if (g_str_equal (tag, known[j]))
              return known[j];
        }
    }

  return "vanilla";
}

/* The newest Minecraft version the project lists, the way the rows of
 * the browser page show it. */
static const char *
project_minecraft_name (AmlModrinthProject *project)
{
  const char *latest = NULL;

  if (project->versions != NULL)
    {
      for (guint i = 0; i < project->versions->len; i++)
        {
          const char *version = g_ptr_array_index (project->versions, i);

          if (latest == NULL || mc_version_cmp (version, latest) > 0)
            latest = version;
        }
    }

  return latest != NULL ? latest : "?";
}

/* ---------------------------------------------------------- *
 * The browser page
 * ---------------------------------------------------------- */

static gboolean
on_search_timeout (gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  self->debounce = 0;
  run_search (self);

  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  if (self->debounce != 0)
    g_source_remove (self->debounce);

  self->debounce = g_timeout_add (SEARCH_DELAY_MS, on_search_timeout, self);
}

static void
on_previous (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  if (self->offset >= RESULTS_PER_PAGE)
    self->offset -= RESULTS_PER_PAGE;

  run_search (self);
}

static void
on_next (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  self->offset += RESULTS_PER_PAGE;
  run_search (self);
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

static void
result_row_activated (AdwActionRow *row G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  AmlModrinthProject *project =
    g_object_get_data (G_OBJECT (row), "aml-project");

  if (project == NULL)
    return;

  g_clear_pointer (&self->project, aml_modrinth_project_free);
  g_clear_pointer (&self->version, aml_modrinth_version_free);
  self->project = aml_modrinth_project_copy (project);

  dialog_details_open (self);
}

static void
results_arrived (AmlDownloadModpackDialog *self, AmlModrinthResults *results)
{
  take_rows_out (&self->rows, ADW_PREFERENCES_GROUP (self->results_group));
  g_clear_pointer (&self->hits, g_ptr_array_unref);
  self->hits = g_ptr_array_new_with_free_func (
      (GDestroyNotify) aml_modrinth_project_free);

  self->total = results != NULL ? results->total_hits : 0;

  if (results != NULL)
    {
      for (guint i = 0; i < results->projects->len; i++)
        {
          AmlModrinthProject *project =
              g_ptr_array_index (results->projects, i);
          GtkWidget *row = adw_action_row_new ();
          GtkWidget *about = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
          GtkWidget *by = gtk_label_new (NULL);
          GtkWidget *count = gtk_label_new (NULL);
          g_autofree char *author = NULL;
          g_autofree char *downloads = NULL;

          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                         project->title);
          adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row),
                                              FALSE);
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                                       project->description);
          adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);

          author = g_strdup_printf ("by %s", project->author);
          gtk_label_set_text (GTK_LABEL (by), author);
          gtk_widget_add_css_class (by, "dim-label");
          gtk_widget_add_css_class (by, "caption");
          gtk_label_set_xalign (GTK_LABEL (by), 0);
          gtk_box_append (GTK_BOX (about), by);

          downloads = g_strdup_printf (
              "%" G_GUINT64_FORMAT " downloads · %s · %s",
              project->downloads, project_minecraft_name (project),
              project_loader_name (project));
          gtk_label_set_text (GTK_LABEL (count), downloads);
          gtk_widget_add_css_class (count, "dim-label");
          gtk_widget_add_css_class (count, "caption");
          gtk_label_set_xalign (GTK_LABEL (count), 0);
          gtk_box_append (GTK_BOX (about), count);

          adw_action_row_add_prefix (ADW_ACTION_ROW (row), about);
          adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row),
                                                 NULL);

          gtk_widget_set_valign (about, GTK_ALIGN_CENTER);

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
        }
    }

  {
    guint shown = results != NULL ? results->projects->len : 0;
    gboolean empty = shown == 0;
    g_autofree char *text = NULL;

    gtk_widget_set_visible (self->results_status, empty);
    gtk_widget_set_visible (self->page_label, !empty);

    text = g_strdup_printf ("Page %u · %u shown of %" G_GUINT64_FORMAT,
                            self->offset / RESULTS_PER_PAGE + 1, shown,
                            self->total);
    gtk_label_set_text (GTK_LABEL (self->page_label), text);

    gtk_widget_set_sensitive (self->previous, self->offset > 0);
    gtk_widget_set_sensitive (self->next,
                              self->offset + shown < self->total);
  }

  set_busy (self, FALSE);
}

static void
run_search (AmlDownloadModpackDialog *self)
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

  set_busy (self, TRUE);

  escaped_query =
    g_uri_escape_string (query != NULL ? query : "", NULL, FALSE);
  escaped_facets = g_uri_escape_string ("[[\"project_type:modpack\"]]",
                                        NULL, FALSE);

  url = g_strdup_printf (
    AML_MODRINTH_URL "/search?query=%s&facets=%s&offset=%u&limit=%u",
    escaped_query, escaped_facets, self->offset, RESULTS_PER_PAGE);

  aml_download_json_async (url, self->search, on_search_done, self);
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
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED))
        return;

      adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                                 "Could not search Modrinth");
      adw_status_page_set_description (ADW_STATUS_PAGE (self->results_status),
                                       error->message);
      results_arrived (self, NULL);
      return;
    }

  answer = json_to_string (json_parser_get_root (parser), FALSE);
  results = aml_modrinth_results_from_json (answer, -1, &error);

  if (results == NULL)
    {
      adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                                 "Could not understand the answer");
      adw_status_page_set_description (
        ADW_STATUS_PAGE (self->results_status), error->message);
      results_arrived (self, NULL);
      return;
    }

  results_arrived (self, results);
}

static void
build_browse_page (AmlDownloadModpackDialog *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *page = NULL;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *bottom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *clamp = adw_clamp_new ();

  self->results_group = adw_preferences_group_new ();

  self->search_entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (self->search_entry, TRUE);
  gtk_search_entry_set_placeholder_text (
    GTK_SEARCH_ENTRY (self->search_entry), "Search modpacks");
  g_signal_connect (self->search_entry, "changed",
                    G_CALLBACK (on_search_changed), self);

  self->spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (self->spinner, 16, 16);
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), TRUE);
  gtk_widget_set_visible (self->spinner, FALSE);

  gtk_box_append (GTK_BOX (top), self->search_entry);
  gtk_box_append (GTK_BOX (top), self->spinner);
  gtk_widget_set_margin_start (top, 12);
  gtk_widget_set_margin_end (top, 12);
  gtk_widget_set_margin_top (top, 6);
  gtk_widget_set_margin_bottom (top, 6);
  gtk_box_append (GTK_BOX (box), top);

  self->results_status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->results_status),
                                 "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                             "Nothing to show yet");
  adw_status_page_set_description (
    ADW_STATUS_PAGE (self->results_status),
    "Type what to look for, or browse everything Modrinth has for this "
    "kind of pack.");
  gtk_box_append (GTK_BOX (box), self->results_status);

  self->results_group = adw_preferences_group_new ();

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  adw_clamp_set_child (ADW_CLAMP (clamp), self->results_group);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), clamp);
  gtk_widget_set_margin_start (clamp, 12);
  gtk_widget_set_margin_end (clamp, 12);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (box), scroller);

  self->results_status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->results_status),
                                 "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                             "Nothing to show yet");
  adw_status_page_set_description (
    ADW_STATUS_PAGE (self->results_status),
    "Search, or browse the whole catalog: they all span what the game "
    "can say.");
  gtk_widget_set_visible (self->results_status, FALSE);
  gtk_box_append (GTK_BOX (box), self->results_status);

  self->previous = gtk_button_new_with_label ("Previous");
  self->next = gtk_button_new_with_label ("Next");
  self->page_label = gtk_label_new ("");
  gtk_widget_add_css_class (self->page_label, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (self->page_label), 0.5);
  gtk_widget_set_hexpand (self->page_label, TRUE);
  gtk_box_append (GTK_BOX (bottom), self->previous);
  gtk_box_append (GTK_BOX (bottom), self->page_label);
  gtk_box_append (GTK_BOX (bottom), self->next);
  gtk_widget_set_margin_start (bottom, 12);
  gtk_widget_set_margin_end (bottom, 12);
  gtk_widget_set_margin_bottom (bottom, 12);
  g_signal_connect (self->previous, "clicked", G_CALLBACK (on_previous),
                    self);
  g_signal_connect (self->next, "clicked", G_CALLBACK (on_next), self);
  gtk_box_append (GTK_BOX (box), bottom);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);

  page = GTK_WIDGET (adw_navigation_page_new (toolbar, "Browse Modpacks"));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (page), "browse");

  adw_navigation_view_add (ADW_NAVIGATION_VIEW (self->nav),
                           ADW_NAVIGATION_PAGE (page));
}

static void
set_busy (AmlDownloadModpackDialog *self, gboolean busy)
{
  gtk_widget_set_visible (self->spinner, busy);
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
 * Page: Details
 * ---------------------------------------------------------- */

static void
build_details_page (AmlDownloadModpackDialog *self)
{
  /* the page itself is built every time; widget lifetimes track it. */
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *page = NULL;
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *heading = adw_preferences_group_new ();
  GtkWidget *title = gtk_label_new (self->project->title);
  GtkWidget *meta = gtk_label_new (NULL);
  GtkWidget *body = gtk_label_new (self->project->description);
  GtkWidget *downloads = gtk_label_new (NULL);
  GtkWidget *versions_group = adw_preferences_group_new ();
  GtkWidget *instance_group = adw_preferences_group_new ();
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *cancel = gtk_button_new_with_label ("Cancel");

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  gtk_widget_add_css_class (title, "title-2");
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), title);

  meta = gtk_label_new (NULL);
  {
    g_autofree char *author = g_strdup_printf ("by %s", self->project->author);

    gtk_label_set_text (GTK_LABEL (meta), author);
  }
  gtk_widget_add_css_class (meta, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (meta), 0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), meta);

  gtk_label_set_wrap (GTK_LABEL (body), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (body), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (body), 0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), body);

  {
    g_autofree char *downloads_str =
      g_strdup_printf ("%" G_GUINT64_FORMAT " downloads", self->project->downloads);

    gtk_label_set_text (GTK_LABEL (downloads), downloads_str);
  }
  gtk_widget_add_css_class (downloads, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (downloads), 0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), downloads);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (versions_group),
                                   "Version");

  self->version_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->version_row),
                                 "Pack");
  adw_combo_row_set_use_subtitle (ADW_COMBO_ROW (self->version_row), TRUE);
  {
    GtkStringList *strings = gtk_string_list_new (NULL);

    if (self->versions != NULL)
      {
        for (guint i = 0; i < self->versions->len; i++)
          {
            AmlModrinthVersion *version =
              g_ptr_array_index (self->versions, i);
            char label[160];

            version_row_label (version, label, sizeof label);
            gtk_string_list_append (strings, label);
          }
      }

    adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row),
                             G_LIST_MODEL (strings));
    g_object_unref (strings);
  }
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (versions_group),
                             self->version_row);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (instance_group),
                                   "Instance");

  GtkWidget *name_entry = adw_entry_row_new ();

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_entry), "Name");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (name_entry), TRUE);
  gtk_editable_set_text (GTK_EDITABLE (name_entry),
                         self->project->title);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (instance_group),
                             name_entry);
  self->name_entry = name_entry;

  g_autofree char *default_directory = g_build_filename (
      g_get_user_data_dir (), "AML", "instances", NULL);

  self->directory_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->directory_row),
                                 "Directory");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->directory_row),
                               default_directory);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (instance_group),
                             self->directory_row);
  GtkWidget *pick_button = gtk_button_new_with_label ("Choose…");

  gtk_widget_add_css_class (pick_button, "flat");
  gtk_widget_set_valign (pick_button, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->directory_row),
                             pick_button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (self->directory_row),
                                         pick_button);
  g_signal_connect (pick_button, "clicked", G_CALLBACK (on_pick_directory),
                    self);

  AdwPreferencesPage *page_less = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  adw_preferences_page_add (page_less, ADW_PREFERENCES_GROUP (heading));
  adw_preferences_page_add (page_less, ADW_PREFERENCES_GROUP (versions_group));
  adw_preferences_page_add (page_less, ADW_PREFERENCES_GROUP (instance_group));

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start (buttons, 12);
  gtk_widget_set_margin_end (buttons, 12);
  gtk_widget_set_margin_bottom (buttons, 12);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);

  cancel = gtk_button_new_with_label ("Cancel");
  GtkWidget *install = gtk_button_new_with_label ("Install");

  gtk_widget_add_css_class (install, "suggested-action");

  self->install_button = install;

  gtk_box_append (GTK_BOX (buttons), cancel);
  gtk_box_append (GTK_BOX (buttons), install);

  g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel_clicked),
                    self);
  g_signal_connect (install, "clicked", G_CALLBACK (on_install_clicked),
                    self);

  gtk_box_append (GTK_BOX (box), GTK_WIDGET (page_less));
  gtk_box_append (GTK_BOX (box), buttons);
  gtk_widget_set_vexpand (GTK_WIDGET (page_less), TRUE);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box);
  gtk_widget_set_vexpand (scroller, TRUE);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), scroller);

  page = GTK_WIDGET (adw_navigation_page_new (toolbar,
                                              self->project->title));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (page), "details");

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                            ADW_NAVIGATION_PAGE (page));
}

/* Get the modpack's list of published versions and show it on the
 * already-pushed details page. */
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
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED))
        return;

      adw_status_page_set_title (
        ADW_STATUS_PAGE (self->results_status), "Could not load versions");
      adw_status_page_set_description (
        ADW_STATUS_PAGE (self->results_status), error->message);
      gtk_widget_set_visible (self->results_status, TRUE);
      return;
    }

  versions = versions_of_answer (json_parser_get_root (parser), &error);

  if (versions != NULL)
    g_ptr_array_sort (versions, by_date_desc);

  if (versions == NULL)
    {
      adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                                 "Could not load versions");
      adw_status_page_set_description (
        ADW_STATUS_PAGE (self->results_status), error->message);
      return;
    }

  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = versions;
  versions = NULL;

  {
    GtkStringList *strings = build_version_strings (self->versions);

    adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row),
                             G_LIST_MODEL (strings));
    g_object_unref (strings);
  }

  gtk_widget_set_sensitive (self->install_button, self->versions->len > 0);
}

static void
dialog_details_open (AmlDownloadModpackDialog *self)
{
  g_autofree char *escaped_id =
    g_uri_escape_string (self->project->project_id, NULL, FALSE);
  g_autofree char *url =
    g_strdup_printf (AML_MODRINTH_URL "/project/%s/version", escaped_id);

  build_details_page (self);

  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);

  g_clear_object (&self->fetch);
  self->fetch = g_cancellable_new ();

  aml_download_json_async (url, self->fetch, on_versions_answer, self);
}


/* ---------------------------------------------------------- *
 * The other pages
 * ---------------------------------------------------------- */

static void
dialog_nav_pop_to_tag (AmlDownloadModpackDialog *self, const char *tag)
{
  if (self == NULL || self->nav == NULL)
    return;

  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (self->nav), tag);
}

static GtkWidget *
build_error_page (AmlDownloadModpackDialog *self,
                  const char *what,
                  const char *message,
                  GCallback retry,
                  GCallback cancel)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *box = NULL;
  GtkWidget *status = adw_status_page_new ();
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *retry_btn = gtk_button_new_with_label ("Retry");
  GtkWidget *back_btn = gtk_button_new_with_label ("Back");
  GtkWidget *page = NULL;

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                 "dialog-error-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), what);
  adw_status_page_set_description (ADW_STATUS_PAGE (status), message);

  gtk_widget_add_css_class (retry_btn, "suggested-action");
  g_signal_connect (retry_btn, "clicked", retry, self);
  g_signal_connect (back_btn, "clicked", cancel, self);
  gtk_box_append (GTK_BOX (buttons), retry_btn);
  gtk_box_append (GTK_BOX (buttons), back_btn);
  gtk_widget_set_halign (buttons, GTK_ALIGN_CENTER);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (box), status);
  gtk_box_append (GTK_BOX (box), buttons);
  gtk_widget_set_vexpand (GTK_WIDGET (status), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (status), GTK_ALIGN_CENTER);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  page = GTK_WIDGET (adw_navigation_page_new (toolbar, "Could not install"));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (page), "error");

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

static GtkWidget *
build_installing_page (AmlDownloadModpackDialog *self)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *status = adw_status_page_new ();
  GtkWidget *page = NULL;
  GtkWidget *bar = gtk_progress_bar_new ();
  GtkWidget *running = gtk_label_new ("Nothing to do yet, coming up now…");
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  self->install_bar = bar;
  self->install_count = running;

  gtk_widget_set_vexpand (GTK_WIDGET (bar), FALSE);
  gtk_widget_add_css_class (bar, "progress");
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), 0.0);
  gtk_widget_set_margin_start (bar, 12);
  gtk_widget_set_margin_end (bar, 12);

  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                 "content-loading-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "Installing…");
  adw_status_page_set_description (ADW_STATUS_PAGE (status), "");

  g_signal_connect (cancel_btn, "clicked", G_CALLBACK (on_install_cancel),
                    self);
  self->install_cancel = cancel_btn;

  gtk_box_append (GTK_BOX (buttons), cancel_btn);
  gtk_widget_set_halign (buttons, GTK_ALIGN_CENTER);

  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_box_append (GTK_BOX (outer), status);
  gtk_box_append (GTK_BOX (outer), bar);
  gtk_box_append (GTK_BOX (outer), running);
  gtk_widget_set_halign (running, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (status, TRUE);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), outer);
  page = GTK_WIDGET (adw_navigation_page_new (toolbar, "Installing"));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (page), "installing");

  return page;
}

/* Handler buttons patched around the pages. */

static void
on_cancel_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  dialog_nav_pop_to_tag (self, "browse");
}

static void
on_error_back (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  dialog_nav_pop_to_tag (self, "details");
}

static void
on_error_retry_fetch (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  self->fail_is_install = FALSE;
  dialog_start_features (self);
}

static void
on_error_retry_install (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  start_install (self);
}

static void
on_install_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  /* get whatever network is doing and put the user back */
  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);

  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (self->nav),
                                  "details");
}

static void
on_pick_directory_selected (GObject *source,
                            GAsyncResult *result,
                            gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), result,
                                          &error);

  if (file == NULL)
    {
      /* dismiss is not failure */
      if (error == NULL ||
          g_error_matches (error, GTK_DIALOG_ERROR,
                           GTK_DIALOG_ERROR_CANCELLED) ||
          g_error_matches (error, GTK_DIALOG_ERROR,
                           GTK_DIALOG_ERROR_DISMISSED))
        return;

      dialog_nav_pop_to_tag (self, "details");
      return;
    }

  g_free (self->directory);
  self->directory = g_file_get_path (file);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->directory_row),
                               self->directory);
}

static void
on_pick_directory (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();

  gtk_file_dialog_select_folder (dialog,
                                 GTK_IS_WINDOW (root) ? GTK_WINDOW (root)
                                                      : NULL,
                                 NULL, on_pick_directory_selected, self);
}

static void
on_install_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));
  guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (self->version_row));

  if (self->versions == NULL || selected >= self->versions->len)
    return;

  if (name == NULL || *name == '\0')
    name = self->project->title;

  g_free (self->instance_name);
  self->instance_name = g_strdup (name);

  if (self->version != NULL)
    aml_modrinth_version_free (self->version);

  self->version = aml_modrinth_version_copy (
      g_ptr_array_index (self->versions, selected));

  dialog_start_features (self);
}

/* ---------------------------------------------------------- *
 * The pack fetched and the pack installed
 * ---------------------------------------------------------- */

static void
on_install_progress (guint done,
                     guint total,
                     const char *path G_GNUC_UNUSED,
                     gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autofree char *label = NULL;

  if (self->install_bar == NULL || self->install_count == NULL)
    return;

  gtk_progress_bar_set_fraction (
      GTK_PROGRESS_BAR (self->install_bar),
      total > 0 ? (double) done / (double) total : 0.0);

  label = g_strdup_printf ("%u of %u files", done, total);
  gtk_label_set_text (GTK_LABEL (self->install_count), label);
}

static void
on_install_done (GObject *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlInstance) instance = NULL;

  instance = aml_modpack_install_finish (result, &error);

  if (instance == NULL)
    {
      if (g_error_matches (error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      self->fail_is_install = TRUE;
      push_error (self, "Could not install the modpack", error->message,
                  G_CALLBACK (on_error_retry_install));
      return;
    }

  adw_dialog_close (ADW_DIALOG (self));
}

static void
start_install (AmlDownloadModpackDialog *self)
{
  g_autoptr (GPtrArray) chosen = NULL;
  const char *name = NULL;
  const char *directory = NULL;

  g_return_if_fail (self->modpack != NULL);

  /* The installing page is a fresh one on every attempt. */
  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (self->nav),
                                  "details");

  GtkWidget *page = build_installing_page (self);

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                            ADW_NAVIGATION_PAGE (page));

  name = self->instance_name;

  if (name == NULL || *name == '\0')
    name = self->modpack->name;

  if (name == NULL || *name == '\0')
    name = self->project != NULL && self->project->title != NULL
           ? self->project->title
           : "modpack";

  directory = self->directory; /* NULL means: the manager's default */

  /* the user's selections for the optional files */
  chosen = g_ptr_array_new ();

  if (self->opt_switch_rows != NULL)
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

  aml_modpack_install_async (self->modpack, self->manager, name, directory,
                             chosen, self->fetch, on_install_progress,
                             on_install_done, self);
}

static GtkWidget *
build_optional_page (AmlDownloadModpackDialog *self,
                     GPtrArray *optional_files)
{
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *heading = adw_preferences_group_new ();
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *cancel = gtk_button_new_with_label ("Cancel");
  GtkWidget *install = gtk_button_new_with_label ("Install");
  GtkWidget *page = NULL;

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (heading),
                                   "Optional files");
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (heading),
    "The pack lists these files but says they are not needed. Nothing of "
    "them is installed unless you ask. Default is off.");

  g_clear_pointer (&self->opt_switch_rows, g_ptr_array_unref);
  self->opt_switch_rows = g_ptr_array_new ();
  g_clear_pointer (&self->opt_files, g_ptr_array_unref);
  self->opt_files = g_ptr_array_new_with_free_func (NULL);

  for (guint i = 0; i < optional_files->len; i++)
    {
      AmlModpackFile *file = g_ptr_array_index (optional_files, i);
      GtkWidget *row = adw_switch_row_new ();

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), file->path);
      adw_switch_row_set_active (ADW_SWITCH_ROW (row), FALSE);
      gtk_widget_add_css_class (row, "dim-label");

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), row);
      g_ptr_array_add (self->opt_switch_rows, row);
      g_ptr_array_add (self->opt_files, file);
    }

  gtk_widget_add_css_class (install, "suggested-action");
  gtk_box_append (GTK_BOX (buttons), cancel);
  gtk_box_append (GTK_BOX (buttons), install);
  gtk_widget_set_margin_start (buttons, 12);
  gtk_widget_set_margin_end (buttons, 12);
  gtk_widget_set_margin_bottom (buttons, 12);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);

  g_signal_connect (cancel, "clicked", G_CALLBACK (on_optional_cancel),
                    self);
  g_signal_connect (install, "clicked", G_CALLBACK (on_optional_install),
                    self);

  AdwPreferencesPage *page_holder =
    ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  adw_preferences_page_add (page_holder, ADW_PREFERENCES_GROUP (heading));

  GtkWidget *boxy = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_box_append (GTK_BOX (boxy), GTK_WIDGET (page_holder));
  gtk_box_append (GTK_BOX (boxy), buttons);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), boxy);
  page = GTK_WIDGET (adw_navigation_page_new (toolbar, "Choose Files"));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (page), "optional");

  return page;
}

static void
on_optional_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;

  dialog_nav_pop_to_tag (self, "details");
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


/* Fetch the .mrpack, parse it, and decide if the user has options. */

static void
on_pack_downloaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  AmlDownloadModpackDialog *self = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_download_file_finish (result, &error))
    {
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED))
        return;

      self->fail_is_install = FALSE;
      push_error (self, "Pack could not be fetched", error->message,
                  G_CALLBACK (on_error_retry_fetch));
      return;
    }

  g_clear_pointer (&self->modpack, aml_modpack_free);
  self->modpack = aml_modpack_new_from_zip (self->pack_path, &error);

  if (self->modpack == NULL)
    {
      self->fail_is_install = FALSE;
      push_error (self, "Invalid pack", error->message,
                  G_CALLBACK (on_error_retry_fetch));
      return;
    }

  if (!aml_modpack_resolve (self->modpack, &error))
    {
      self->fail_is_install = FALSE;
      push_error (self, "The pack does not define its install",
                  error->message, G_CALLBACK (on_error_retry_fetch));
      return;
    }

  {
    g_autoptr (GPtrArray) optional =
      aml_modpack_optional_client_files (self->modpack);

    if (optional->len > 0)
      {
        GtkWidget *page = build_optional_page (self, optional);

        adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                                  ADW_NAVIGATION_PAGE (page));
        return;
      }
  }

  start_install (self);
}

static void
dialog_start_features (AmlDownloadModpackDialog *self)
{
  AmlModrinthFile *file = NULL;
  GtkWidget *page = NULL;

  if (self->version == NULL || self->version->files == NULL)
    return;

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
      return;
    }

  page = build_installing_page (self);

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                            ADW_NAVIGATION_PAGE (page));

  /* Reset automatic labelling on each attempt. */
  if (self->install_count != NULL)
    gtk_label_set_text (GTK_LABEL (self->install_count),
                        "Downloading the pack…");

  {
    g_autofree char *cache_dir = g_build_filename (
      g_get_user_cache_dir (), "AML", "packs", NULL);
    const char *name = "pack.mrpack";

    g_mkdir_with_parents (cache_dir, 0755);

    if (file->filename != NULL && *file->filename != '\0')
      name = file->filename;

    self->pack_path = g_build_filename (cache_dir, name, NULL);

    aml_download_file_async (file->url, self->pack_path, file->sha1,
                             G_CHECKSUM_SHA1, self->fetch, NULL,
                             on_pack_downloaded, self);
  }
}
