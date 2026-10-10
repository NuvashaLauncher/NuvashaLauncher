/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-page-private.h"
#include "aml-content-format.h"
#include "aml-i18n.h"
#include "aml-markdown.h"

/* One answer of the page of a project while what it shows is being
 * read: the page that opened it, weakly held, and the project the
 * versions are asked for. The page can be closed while a request is
 * still running, so the answer does nothing instead of touching memory
 * that is gone. */
typedef struct {
  AmlContentPage *page; /* weak */
  char *project_id;     /* owned */
} DetailsWatch;

static DetailsWatch *
details_watch_new (AmlContentPage *self, const char *project_id)
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
  AmlContentPage *self = user_data;
  AmlModrinthVersion *version = NULL;

  /* The list is what moved the checkmark itself. */
  if (g_object_get_data (G_OBJECT (check), "aml-setting") != NULL)
    return;

  version = selected_version (self->project.details_dropdown, self->project.details_versions);

  if (version != NULL)
    {
      if (gtk_check_button_get_active (check))
        pick_add (self, version);
      else
        pick_remove (self, version);
    }

  /* What the list holds is what decides the state of the button, so a
     click that could not be carried out leaves it where it was. */
  aml_content_page_refresh_picks (self);
}

/* Another version is what the dropdown shows: the checkmark at its left
   says whether that one is the one on the list. */
static void
on_version_selected (GObject *object G_GNUC_UNUSED,
                     GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  refresh_pick_check (self);
  aml_content_page_refresh_picks (self);
}

/* Internal project-details workflow for AmlContentPage. */

/* ---------------------------------------------------------- *
 * The page of one project
 * ---------------------------------------------------------- */



/* What the download button would take: the version the dropdown shows,
 * or NULL when none is chosen yet. */
AmlModrinthVersion *
selected_version (GtkWidget *dropdown, GPtrArray *versions)
{
  guint index = gtk_drop_down_get_selected (GTK_DROP_DOWN (dropdown));

  if (index == GTK_INVALID_LIST_POSITION || versions == NULL ||
      index >= versions->len)
    return NULL;

  return g_ptr_array_index (versions, index);
}

/* What a version is called: the name Modrinth writes, which already
 * holds the version itself. */
const char *
version_name_of (AmlModrinthVersion *version)
{
  if (version->name != NULL && *version->name != '\0')
    return version->name;

  if (version->version_number != NULL && *version->version_number != '\0')
    return version->version_number;

  return version->version_id != NULL ? version->version_id : "the file";
}

/* The one button under the versions: it installs what the dropdown
 * shows, and the page it was in has done its work. */
static void
on_version_download_clicked (GtkButton *button, gpointer user_data)
{
  AmlContentPage *self = user_data;
  GtkWidget *dropdown = g_object_get_data (G_OBJECT (button), "aml-dropdown");
  GPtrArray *versions = g_object_get_data (G_OBJECT (button), "aml-versions");
  AmlModrinthVersion *version = NULL;

  if (self == NULL || dropdown == NULL || versions == NULL)
    return;

  version = selected_version (dropdown, versions);

  if (version == NULL)
    return;

  /* The page stays where it is: the install is planned and confirmed
     over the list it was asked from, and the files land in the folder
     while the user keeps reading the page. */
  set_busy (self, TRUE);
  start_plan (self, aml_modrinth_version_copy (version),
              self->project.current_project_title != NULL
                ? self->project.current_project_title : version_name_of (version),
              NULL, FALSE);
}

/* What a version reads as in the dropdown: its name and the day it was
 * published, so two versions of one project can be told apart. */
static char *
version_label (AmlModrinthVersion *version)
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
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_list_item_set_child (item, label);
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
static GtkWidget *
loading_child (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *spinner = adw_spinner_new ();
  GtkWidget *label = gtk_label_new (_("Loading…"));

  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (box), spinner);
  gtk_box_append (GTK_BOX (box), label);

  return box;
}

/* Closes the page of the open project, leaving the list the whole width
 * of the page again. An answer that is still on its way finds no page to
 * fill and does nothing. The list row of the project is the way in and
 * the way out, so nothing here has to stand above the project and push
 * its name down. */
void
aml_content_page_close_detail (AmlContentPage *self)
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

/* The whole content of the page, built in one go from what the answers
 * said. Nothing here is filled in as it arrives: the page shows a
 * spinner until this is called, so the meta, count, description and
 * versions appear together. */
static GtkWidget *
details_content (AmlContentPage *self)
{
  GtkWidget *content = NULL;
  GtkWidget *scroller = NULL;
  GtkWidget *box = NULL;
  GtkWidget *heading = NULL;
  GtkWidget *bar = NULL;
  GtkWidget *title = NULL;
  GtkWidget *meta = NULL;
  GtkWidget *count = NULL;
  g_autofree char *downloads = NULL;

  scroller = gtk_scrolled_window_new ();
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
      text = aml_content_format_author (self->project.details->license, who);
    else
      text = aml_content_format_author (NULL, who);

    meta = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (meta), 0);
    gtk_widget_add_css_class (meta, "dim-label");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), meta);
  }

  downloads = aml_content_format_downloads (self->project.current_project_downloads);
  count = gtk_label_new (downloads);
  gtk_label_set_xalign (GTK_LABEL (count), 0);
  gtk_widget_add_css_class (count, "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), count);

  /* The long description, in Markdown: headings, links and images are
     read and shown as widgets rather than as the raw text. */
  watch_widget (&self->project.details_body, gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));

  if (self->project.details_problem != NULL)
    gtk_box_append (GTK_BOX (self->project.details_body),
                    plain_label (self->project.details_problem));
  else if (self->project.details != NULL && self->project.details->body != NULL &&
           *self->project.details->body != '\0')
    aml_markdown_render (GTK_BOX (self->project.details_body), self->project.details->body);
  else if (self->project.details != NULL)
    gtk_box_append (GTK_BOX (self->project.details_body),
                    plain_label (self->project.details->description));

  gtk_box_append (GTK_BOX (box), heading);
  gtk_box_append (GTK_BOX (box), self->project.details_body);

  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box);
  gtk_widget_set_vexpand (scroller, TRUE);

  /* The foot: every version this instance can take, the checkmark that
     puts the one shown on the list the button at the foot of the other
     page installs, and the button that installs this one on its own.
     The newest is what the dropdown shows from the start. */
  watch_widget (&self->project.details_check, gtk_check_button_new ());
  gtk_widget_set_valign (self->project.details_check, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (self->project.details_check, FALSE);
  gtk_widget_set_tooltip_text (self->project.details_check,
                               _("Install this version with the selected "
                                 "ones"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->project.details_check),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Install this version with the selected "
                                    "ones"), -1);
  g_signal_connect (self->project.details_check, "toggled",
                    G_CALLBACK (on_version_picked), self);

  watch_widget (&self->project.details_dropdown, gtk_drop_down_new (NULL, NULL));
  {
    GtkListItemFactory *factory = version_factory ();

    gtk_drop_down_set_factory (GTK_DROP_DOWN (self->project.details_dropdown), factory);
    gtk_drop_down_set_list_factory (GTK_DROP_DOWN (self->project.details_dropdown),
                                    factory);
    g_object_unref (factory);
  }
  g_signal_connect (self->project.details_dropdown, "notify::selected",
                    G_CALLBACK (on_version_selected), self);
  gtk_widget_set_hexpand (self->project.details_dropdown, TRUE);
  gtk_widget_set_valign (self->project.details_dropdown, GTK_ALIGN_CENTER);

  watch_widget (&self->project.details_download, gtk_button_new_with_label (_("Download")));
  gtk_widget_add_css_class (self->project.details_download, "suggested-action");
  gtk_widget_set_valign (self->project.details_download, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (self->project.details_download, FALSE);
  g_signal_connect (self->project.details_download, "clicked",
                    G_CALLBACK (on_version_download_clicked), self);

  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (bar), self->project.details_check);
  gtk_box_append (GTK_BOX (bar), self->project.details_dropdown);
  gtk_box_append (GTK_BOX (bar), self->project.details_download);
  gtk_widget_set_margin_top (bar, 12);
  gtk_widget_set_margin_bottom (bar, 12);
  gtk_widget_set_margin_start (bar, 12);
  gtk_widget_set_margin_end (bar, 12);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), scroller);
  gtk_box_append (GTK_BOX (content),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (content), bar);

  return content;
}

/* Puts the real content in the page once both the description and the
 * versions are in. */
static void
detail_fill (AmlContentPage *self)
{
  if (self == NULL || self->project.details_page == NULL)
    return;

  if (!self->project.details_ready || !self->project.versions_ready)
    return;

  adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self->project.details_page),
                                 details_content (self));

  if (self->project.versions_problem != NULL)
    show_toast (self, self->project.versions_problem);
  else if (self->project.details_versions != NULL && self->project.details_versions->len == 0)
    show_toast (self, _("This project publishes nothing that fits this instance."));

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
      /* The newest version is what a user most often wants, so it is
         what the dropdown shows from the start. */
      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->project.details_dropdown), 0);
      g_object_unref (labels);

      /* The button owns the versions from here, so the install it starts
         does not depend on the page still holding them. */
      g_object_set_data_full (G_OBJECT (self->project.details_download), "aml-versions",
                              g_ptr_array_ref (self->project.details_versions),
                              (GDestroyNotify) g_ptr_array_unref);
      g_object_set_data (G_OBJECT (self->project.details_download), "aml-dropdown",
                         self->project.details_dropdown);
    }

  {
    gboolean have_versions = self->project.details_versions != NULL &&
                             self->project.details_versions->len > 0;

    gtk_widget_set_sensitive (self->project.details_download,
                              have_versions && !self->installed.identifying);
    /* Nothing to put on the list while the dropdown shows none. */
    gtk_widget_set_sensitive (self->project.details_check,
                              have_versions && !self->installed.identifying);
  }

  /* The version the dropdown shows from the start is not on the list
     until the checkmark says so. */
  aml_content_page_refresh_picks (self);
}

static void
on_detail_versions_ready (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result, gpointer user_data)
{
  DetailsWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  versions = aml_modrinth_versions_finish (result, &error);

  if (self == NULL)
    {
      details_watch_free (watch);
      return;
    }

  /* Another project was opened while this answer was on its way: the
     project that is open now must not be filled with this one. */
  if (g_strcmp0 (self->project.current_project_id, watch->project_id) != 0)
    {
      details_watch_free (watch);
      return;
    }

  set_busy (self, FALSE);

  if (versions == NULL)
    {
      /* What a project publishes for nothing that fits is said in the
       * words the page shows everywhere else. */
      if (error != NULL && g_error_matches (error, AML_MODRINTH_ERROR,
                                            AML_MODRINTH_ERROR_NOT_FOUND))
        self->project.versions_problem = g_strdup (error->message);
      else
        self->project.versions_problem = g_strdup (_("no compatible version"));
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
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthDetails) details = NULL;
  g_autofree char *author = NULL;

  details = aml_modrinth_details_finish (result, &error);

  /* The dialog can be closed while its details are on their way; the
     answer then fills nothing. */
  if (self == NULL)
    {
      details_watch_free (watch);
      return;
    }

  /* Another project was opened while this answer was on its way: the
     project that is open now must not be filled with this one. */
  if (g_strcmp0 (self->project.current_project_id, watch->project_id) != 0)
    {
      details_watch_free (watch);
      return;
    }

  if (details == NULL)
    {
      set_busy (self, FALSE);

      /* The reason is shown where the body would have been, on the
         dialog that is already open, and not only in a toast that may be
         behind it. */
      if (self->project.details_problem == NULL)
        self->project.details_problem =
          g_strdup (error != NULL ? error->message
                                  : "the project cannot be read");
    }
  else
    {
      g_clear_pointer (&self->project.details, aml_modrinth_details_free);
      self->project.details = g_steal_pointer (&details);
    }

  self->project.details_ready = TRUE;
  detail_fill (self);

  /* Only versions that fit this instance are asked for, and only those
     can reach the list. The watch goes on to the next answer, which is
     the one that frees it. */
  aml_modrinth_versions_async (watch->project_id, self->type,
                               self->minecraft_version, self->loader,
                               self->browse.model->cancellable, on_detail_versions_ready, watch);
}

/* The dialog of @project: what it is, every version of it that fits this
 * instance, and the one button that installs the version chosen from
 * them. It is opened by activating a result row, so the version is
 * always chosen by the user and never picked unseen. The long
 * description is Markdown and is shown as such. */
void
open_project_page (AmlContentPage *self, AmlModrinthProject *project)
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
  g_clear_pointer (&self->project.details, aml_modrinth_details_free);
  g_clear_pointer (&self->project.details_versions, g_ptr_array_unref);
  g_free (self->project.details_problem);
  self->project.details_problem = NULL;
  g_free (self->project.versions_problem);
  self->project.versions_problem = NULL;

  /* The project opens in the right half of the page, beside the list it
     was picked from, showing a spinner until both the description and
     the versions are in. Its name is the first thing the page shows. */
  page = GTK_WIDGET (adw_navigation_page_new (loading_child (),
                                              project->title));
  watch_widget (&self->project.details_page, page);

  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), ADW_NAVIGATION_PAGE (page));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->project.split), FALSE);

  set_busy (self, TRUE);
  aml_modrinth_details_async (project->project_id, self->browse.model->cancellable,
                              on_details_ready,
                              details_watch_new (self, project->project_id));
}

/* A click on a row opens its project in the other half. A click on the
 * row of the project that is already open closes it again: the list is
 * the way in and the way out, so no button has to stand over the project
 * and push it down. */
void
aml_content_page_on_result_activated (AdwActionRow *row, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlModrinthProject *project =
    g_object_get_data (G_OBJECT (row), "aml-project");

  if (project == NULL)
    return;

  if (self->project.details_page != NULL &&
      g_strcmp0 (self->project.current_project_id, project->project_id) == 0)
    {
      aml_content_page_close_detail (self);
      return;
    }

  open_project_page (self, project);
}

