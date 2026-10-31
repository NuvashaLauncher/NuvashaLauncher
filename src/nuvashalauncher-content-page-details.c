/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-format.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-markdown.h"
#include "nuvashalauncher-operation.h"
#include "nuvashalauncher-ui-feedback.h"

/* Tracks one asynchronous project-details request. The page is weakly
 * referenced because it can close before the request finishes. */
typedef struct {
  nuvashalauncherContentPage *page; /* weak */
  char *project_id;     /* owned */

  /* Handle for the request currently associated with this watch. */
  nuvashalauncherTaskScope *operation;
} DetailsWatch;

static DetailsWatch *
details_watch_new (nuvashalauncherContentPage *self, const char *project_id)
{
  DetailsWatch *watch = g_new0 (DetailsWatch, 1);

  watch->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &watch->page);
  watch->project_id = g_strdup (project_id);

  return watch;
}

static void
details_watch_free (DetailsWatch *watch)
{
  if (watch->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (watch->page),
                                  (gpointer *) &watch->page);

  if (watch->operation != NULL)
    nuvashalauncher_operation_unref (watch->operation);

  g_free (watch->project_id);
  g_free (watch);
}

/* The checkmark at the left of the versions was clicked: the version the
   dropdown shows joins or leaves the list. There is nothing to pick while
   the dropdown shows none, which is the case on a project that publishes
   nothing for this instance. */
static void
on_version_picked (GtkCheckButton *check, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  nuvashalauncherModrinthVersion *version = NULL;

  /* The list is what moved the checkmark itself. */
  if (g_object_get_data (G_OBJECT (check), "nuvashalauncher-setting") != NULL)
    return;

  version = selected_version (self->project.details_dropdown, self->project.details_versions);

  if (version != NULL)
    {
      if (gtk_check_button_get_active (check))
        pick_add (self, version);
      else
        pick_remove (self, version);
    }

  /* The selected list controls the button state. */
  nuvashalauncher_content_page_refresh_picks (self);
}

/* Update the selected version and its checkmark. */
static void
on_version_selected (GObject *object G_GNUC_UNUSED,
                     GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);

  refresh_pick_check (self);
  nuvashalauncher_content_page_refresh_picks (self);
}

/* Internal project-details workflow for nuvashalauncherContentPage. */

/* ---------------------------------------------------------- *
 * The page of one project
 * ---------------------------------------------------------- */



/* Return the version selected in the download dropdown, or NULL. */
nuvashalauncherModrinthVersion *
selected_version (GtkDropDown *dropdown, GPtrArray *versions)
{
  guint index = gtk_drop_down_get_selected (GTK_DROP_DOWN (dropdown));

  if (index == GTK_INVALID_LIST_POSITION || versions == NULL ||
      index >= versions->len)
    return NULL;

  return g_ptr_array_index (versions, index);
}

/* Return the best available display name for a version. */
const char *
version_name_of (nuvashalauncherModrinthVersion *version)
{
  if (version->name != NULL && *version->name != '\0')
    return version->name;

  if (version->version_number != NULL && *version->version_number != '\0')
    return version->version_number;

  return version->version_id != NULL ? version->version_id : "the file";
}

/* Start installing the version selected in the dropdown. */
static void
on_version_download_clicked (GtkButton *button, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  GtkWidget *dropdown =
    GTK_WIDGET (g_object_get_data (G_OBJECT (button), "nuvashalauncher-dropdown"));
  GPtrArray *versions = g_object_get_data (G_OBJECT (button), "nuvashalauncher-versions");
  nuvashalauncherModrinthVersion *version = NULL;

  if (self == NULL || dropdown == NULL || versions == NULL)
    return;

  version = selected_version (GTK_DROP_DOWN (dropdown), versions);

  if (version == NULL)
    return;

  /* The page stays where it is: the install is planned and confirmed
     over the list it was asked from, and the files land in the folder
     while the user keeps reading the page. */
  set_busy (self, TRUE);
  start_plan (self, nuvashalauncher_modrinth_version_copy (version),
              self->project.current_project_title != NULL
                ? self->project.current_project_title : version_name_of (version),
              NULL, FALSE);
}

/* Format a version label with its publication date. */
static char *
version_label (nuvashalauncherModrinthVersion *version)
{
  if (version->date_published != NULL && *version->date_published != '\0')
    {
      g_autofree char *when = g_strndup (version->date_published, 10);

      return g_strdup_printf ("%s  ·  %s", version_name_of (version), when);
    }

  return g_strdup (version_name_of (version));
}

/* The label of one version in the dropdown. It ellipsizes, so a long
 * name of a version does not set the width of the half the dropdown
 * lives in: the version that is loaded and the spinner that stands there
 * while it is on its way are then the same width. This is what the
 * closed dropdown shows: the version alone, with nothing to click. */
static void
version_item_setup (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                    GtkListItem *item, gpointer user_data G_GNUC_UNUSED)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (NULL));

  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_list_item_set_child (item, GTK_WIDGET (label));
}

static void
version_item_bind (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                   GtkListItem *item, gpointer user_data G_GNUC_UNUSED)
{
  GtkWidget *label = gtk_list_item_get_child (item);
  GtkStringObject *object = gtk_list_item_get_item (item);

  gtk_label_set_text (GTK_LABEL (label),
                      object != NULL ? gtk_string_object_get_string (object)
                                     : "");
}

static GtkListItemFactory *
version_factory (void)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();

  g_signal_connect (factory, "setup", G_CALLBACK (version_item_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (version_item_bind), NULL);

  return factory;
}

/* The words the page shows while the description and the versions are
 * on their way. */
/* Close the project details view and return to the list. A late response is
 * ignored when the details view no longer exists. */
void
nuvashalauncher_content_page_close_detail (nuvashalauncherContentPage *self)
{
  if (self->project.split == NULL)
    return;

  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), NULL);
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), TRUE);
  watch_widget (&self->project.details_page, NULL);
  g_clear_pointer (&self->project.current_project_id, g_free);
}

/* Build the project details after its metadata and versions are available. */
static GtkWidget *
details_content (nuvashalauncherContentPage *self)
{
  GtkWidget *content = NULL;
  GtkWidget *scroller = NULL;
  GtkWidget *box = NULL;
  GtkWidget *heading = NULL;
  g_autoptr (GtkBuilder) details_builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-content-details.ui");
  GtkWidget *title = NULL;
  GtkWidget *meta = NULL;
  GtkWidget *count = NULL;
  g_autofree char *downloads = NULL;

  content = GTK_WIDGET (g_object_ref (
    gtk_builder_get_object (details_builder, "content")));
  scroller = GTK_WIDGET (gtk_builder_get_object (details_builder, "scroller"));
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  heading = adw_preferences_group_new ();

  /* The title, then the license and author and the download count, the
     same lines the other contents show. */
  title = gtk_label_new (self->project.current_project_title);
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_add_css_class (title, "title-2");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), title);

  {
    const char *who = self->project.details_author != NULL &&
                      *self->project.details_author != '\0'
                        ? self->project.details_author
                        : "";
    g_autofree char *text = NULL;

    if (self->project.details != NULL && self->project.details->license != NULL &&
        *self->project.details->license != '\0')
      text = nuvashalauncher_content_format_author (self->project.details->license, who);
    else
      text = nuvashalauncher_content_format_author (NULL, who);

    meta = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (meta), 0);
    gtk_widget_add_css_class (meta, "dim-label");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), meta);
  }

  downloads = nuvashalauncher_content_format_downloads (self->project.current_project_downloads);
  count = gtk_label_new (downloads);
  gtk_label_set_xalign (GTK_LABEL (count), 0);
  gtk_widget_add_css_class (count, "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), count);

  /* The long description, in Markdown: headings and links are read and shown
     as widgets rather than as the raw text. */
  watch_widget (&self->project.details_body, gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));

  if (self->project.details_problem != NULL)
    gtk_box_append (GTK_BOX (self->project.details_body),
                    plain_label (self->project.details_problem));
  else if (self->project.details != NULL && self->project.details->body != NULL &&
           *self->project.details->body != '\0')
    nuvashalauncher_markdown_render (GTK_BOX (self->project.details_body), self->project.details->body);
  else if (self->project.details != NULL)
    gtk_box_append (GTK_BOX (self->project.details_body),
                    plain_label (self->project.details->description));

  gtk_box_append (GTK_BOX (box), heading);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->project.details_body));

  gtk_widget_set_margin_top (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (box), 18);
  gtk_widget_set_margin_end (GTK_WIDGET (box), 18);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (box));
  gtk_widget_set_vexpand (scroller, TRUE);

  /* Show the available versions, the selected version, and the install
     action. The newest version is selected initially. */
   watch_widget (&self->project.details_check,
                 GTK_WIDGET (gtk_builder_get_object (details_builder, "details-check")));
  gtk_widget_set_valign (GTK_WIDGET (self->project.details_check), GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (GTK_WIDGET (self->project.details_check), FALSE);
   g_signal_connect_object (self->project.details_check, "toggled",
                           G_CALLBACK (on_version_picked), self, 0);

   watch_widget (&self->project.details_dropdown,
                 GTK_WIDGET (gtk_builder_get_object (details_builder, "details-dropdown")));
  {
    GtkListItemFactory *factory = version_factory ();

    gtk_drop_down_set_factory (GTK_DROP_DOWN (self->project.details_dropdown), factory);
    gtk_drop_down_set_list_factory (GTK_DROP_DOWN (self->project.details_dropdown),
                                    factory);
    g_object_unref (factory);
  }
  g_signal_connect_object (self->project.details_dropdown, "notify::selected",
                           G_CALLBACK (on_version_selected), self, 0);
  gtk_widget_set_hexpand (GTK_WIDGET (self->project.details_dropdown), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (self->project.details_dropdown), GTK_ALIGN_CENTER);

   watch_widget (&self->project.details_download,
                 GTK_WIDGET (gtk_builder_get_object (details_builder, "details-download")));
  gtk_widget_set_valign (GTK_WIDGET (self->project.details_download), GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (GTK_WIDGET (self->project.details_download), FALSE);
  g_signal_connect_object (self->project.details_download, "clicked",
                           G_CALLBACK (on_version_download_clicked), self, 0);

   return content;
}

/* Puts the real content in the page once both the description and the
 * versions are in. */
static void
detail_fill (nuvashalauncherContentPage *self)
{
  if (self == NULL || self->project.details_page == NULL)
    return;

  if (!self->project.details_ready || !self->project.versions_ready)
    return;

  {
    GtkWidget *content = details_content (self);

    adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self->project.details_page),
                                   content);
    g_object_unref (content);
  }

  if (self->project.versions_problem != NULL)
    show_toast (self, self->project.versions_problem);
  else if (self->project.details_versions != NULL && self->project.details_versions->len == 0)
    {
      g_autofree char *message = g_strdup_printf (
        _("No compatible version found for Minecraft %s."), self->minecraft_version);
      show_toast (self, message);
    }

  if (self->project.details_versions != NULL && self->project.details_versions->len > 0)
    {
      GtkStringList *labels = gtk_string_list_new (NULL);

      for (guint i = 0; i < self->project.details_versions->len; i++)
        {
          g_autofree char *label =
            version_label (g_ptr_array_index (self->project.details_versions, i));

          gtk_string_list_append (labels, label);
        }

      gtk_drop_down_set_model (GTK_DROP_DOWN (self->project.details_dropdown),
                               G_LIST_MODEL (labels));
      /* Select the newest version by default. */
      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->project.details_dropdown), 0);
      g_object_unref (labels);

      /* The button owns the versions from here, so the install it starts
         does not depend on the page still holding them. */
      g_object_set_data_full (G_OBJECT (self->project.details_download), "nuvashalauncher-versions",
                              g_ptr_array_ref (self->project.details_versions),
                              (GDestroyNotify) g_ptr_array_unref);
      g_object_set_data (G_OBJECT (self->project.details_download), "nuvashalauncher-dropdown",
                         self->project.details_dropdown);
    }

  {
    gboolean have_versions = self->project.details_versions != NULL &&
                             self->project.details_versions->len > 0;

    gtk_widget_set_sensitive (GTK_WIDGET (self->project.details_download),
                              have_versions && !self->installed.identifying);
      /* Disable selection when no versions are available. */
    gtk_widget_set_sensitive (GTK_WIDGET (self->project.details_check),
                              have_versions && !self->installed.identifying);
  }

  /* The default version is not selected until its checkmark is activated. */
  nuvashalauncher_content_page_refresh_picks (self);
}

static void
on_detail_versions_ready (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result, gpointer user_data)
{
  DetailsWatch *watch = user_data;
  nuvashalauncherContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  versions = nuvashalauncher_modrinth_versions_finish (result, &error);

  if (self == NULL)
    {
      details_watch_free (watch);
      return;
    }

  /* Ignore results for a project that is no longer open. */
  if (g_strcmp0 (self->project.current_project_id, watch->project_id) != 0)
    {
      details_watch_free (watch);
      return;
    }

  set_busy (self, FALSE);

  if (versions == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        self->project.versions_problem = g_strdup (
          error != NULL ? nuvashalauncher_error_message (error) : _("Could not load versions. Try again."));
      if (error != NULL && !nuvashalauncher_error_is_cancelled (error))
        show_error (self, _("Could not load versions. Try again."), error);
    }
  else
    {
      self->project.details_versions = g_steal_pointer (&versions);
    }

  self->project.versions_ready = TRUE;
  detail_fill (self);
  details_watch_free (watch);
}

static void
on_details_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  DetailsWatch *watch = user_data;
  nuvashalauncherContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModrinthDetails) details = NULL;
  g_autofree char *author = NULL;

  details = nuvashalauncher_modrinth_details_finish (result, &error);

  /* The dialog can be closed while its details are on their way; the
     answer then fills nothing. */
  if (self == NULL)
    {
      details_watch_free (watch);
      return;
    }

  /* Ignore results for a project that is no longer open. */
  if (g_strcmp0 (self->project.current_project_id, watch->project_id) != 0)
    {
      details_watch_free (watch);
      return;
    }

  if (details == NULL)
    {
      set_busy (self, FALSE);

      /* Show the error in the open details page rather than only in a toast. */
      if (self->project.details_problem == NULL)
        self->project.details_problem =
          g_strdup (error != NULL ? nuvashalauncher_error_message (error)
                                   : _("Could not load project details. Try again."));
      if (error != NULL && !nuvashalauncher_error_is_cancelled (error))
        show_error (self, _("Could not search Modrinth"), error);
    }
  else
    {
      g_clear_pointer (&self->project.details, nuvashalauncher_modrinth_details_free);
      self->project.details = g_steal_pointer (&details);
    }

  self->project.details_ready = TRUE;
  detail_fill (self);

  /* Fetch compatible versions next, reusing the request watch. */
  nuvashalauncher_operation_unref (watch->operation);
  watch->operation = nuvashalauncher_modrinth_versions_async (
    watch->project_id, self->type, self->minecraft_version, self->loader,
    on_detail_versions_ready, watch);
  nuvashalauncher_operation_follow (watch->operation,
                                     self->browse.model->cancellable);
}

/* Open the details page for @project and its compatible versions. The user
 * chooses the version; the description is rendered as Markdown. */
void
open_project_page (nuvashalauncherContentPage *self, nuvashalauncherModrinthProject *project)
{
  GtkWidget *page = NULL;

  if (project == NULL)
    return;

  /* What the page needs to rebuild its heading once the answers are in.
     The page itself is built with a spinner and filled later. The id is
     kept so a second click on the same row closes it again. */
  g_free (self->project.current_project_title);
  self->project.current_project_title = g_strdup (project->title);
  g_free (self->project.current_project_id);
  self->project.current_project_id = g_strdup (project->project_id);
  self->project.current_project_downloads = project->downloads;
  g_free (self->project.details_author);
  self->project.details_author = g_strdup (project->author);

  self->project.details_ready = FALSE;
  self->project.versions_ready = FALSE;
  g_clear_pointer (&self->project.details, nuvashalauncher_modrinth_details_free);
  g_clear_pointer (&self->project.details_versions, g_ptr_array_unref);
  g_free (self->project.details_problem);
  self->project.details_problem = NULL;
  g_free (self->project.versions_problem);
  self->project.versions_problem = NULL;

  /* The project opens in the right half of the page, beside the list it
     was picked from, showing a spinner until both the description and
     the versions are in. Its name is the first thing the page shows. */
  page = GTK_WIDGET (adw_navigation_page_new (nuvashalauncher_ui_loading_child (),
                                              project->title));
  watch_widget (&self->project.details_page, page);

  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), ADW_NAVIGATION_PAGE (page));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), FALSE);

  set_busy (self, TRUE);
  {
    DetailsWatch *watch = details_watch_new (self, project->project_id);

    watch->operation =
      nuvashalauncher_modrinth_details_async (project->project_id, on_details_ready,
                                  watch);
    nuvashalauncher_operation_follow (watch->operation,
                                       self->browse.model->cancellable);
  }
}

/* Open the selected project, or close its details view if it is already open. */
void
nuvashalauncher_content_page_on_result_activated (AdwActionRow *row, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  nuvashalauncherModrinthProject *project =
    g_object_get_data (G_OBJECT (row), "nuvashalauncher-project");

  if (project == NULL)
    return;

  if (self->project.details_page != NULL &&
      g_strcmp0 (self->project.current_project_id, project->project_id) == 0)
    {
      nuvashalauncher_content_page_close_detail (self);
      return;
    }

  open_project_page (self, project);
}
