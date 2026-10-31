/* Modrinth modpack details and version selection. */
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

static void show_details_error (nuvashalauncherDownloadModpackDialog *, const char *);

/* ---------------------------------------------------------- *
 * Versions of the details page
 * ---------------------------------------------------------- */

static void
version_row_label (nuvashalauncherModrinthVersion *version, char *out, guint size)
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
  const nuvashalauncherModrinthVersion *va = *(const nuvashalauncherModrinthVersion *const *) a;
  const nuvashalauncherModrinthVersion *vb = *(const nuvashalauncherModrinthVersion *const *) b;

  return g_strcmp0 (vb->date_published, va->date_published);
}

static GtkStringList *
build_version_strings (GPtrArray *versions)
{
  GtkStringList *strings = gtk_string_list_new (NULL);

  for (guint i = 0; i < versions->len; i++)
    {
      nuvashalauncherModrinthVersion *version = g_ptr_array_index (versions, i);
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
      g_set_error_literal (error, nuvashalauncher_MODRINTH_ERROR,
                           nuvashalauncher_MODRINTH_ERROR_PARSE,
                           "the answer is not a list of versions");
      return NULL;
    }

  versions = g_ptr_array_new_with_free_func ((GDestroyNotify)
    nuvashalauncher_modrinth_version_free);

  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *element = json_array_get_element (array, i);
      g_autofree char *json = nuvashalauncher_modrinth_json_node_to_string (element);
      nuvashalauncherModrinthVersion *version =
        nuvashalauncher_modrinth_version_from_json (json, -1, NULL);

      if (version == NULL)
        continue;

      {
        gboolean is_a_pack = FALSE;

        for (guint j = 0;
             version->files != NULL && j < version->files->len;
             j++)
          {
            nuvashalauncherModrinthFile *file = g_ptr_array_index (version->files, j);

            if (file->filename != NULL &&
                g_str_has_suffix (file->filename, ".mrpack"))
              is_a_pack = TRUE;
          }

        if (!is_a_pack)
          {
            nuvashalauncher_modrinth_version_free (version);
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

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);

  return GTK_WIDGET (label);
}

/* The words the dialog shows while the description and the versions are
   on their way. */
/* Close the details view and return to the project list. */
void
close_details (nuvashalauncherDownloadModpackDialog *self)
{
  if (self->split == NULL)
    return;

  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), NULL);
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), TRUE);
  details_watch (&self->details_page, NULL);

  /* Keep the browser at its wide size even when the details half is closed,
     matching the other content browsers. */
}

/* Puts @body in the right half of the browser, beside the list, and takes
 * the whole width of the list back to half. A pack shown before is
 * replaced. */
static void
show_details_page (nuvashalauncherDownloadModpackDialog *self, GtkWidget *body)
{
  AdwNavigationPage *page = ADW_NAVIGATION_PAGE (adw_navigation_page_new (body,
                                                         self->project->title));

  details_watch (&self->details_page, GTK_WIDGET (page));
  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), ADW_NAVIGATION_PAGE (page));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), FALSE);

  /* The dialog is already wide enough for both halves. */
}

/* The author the search answer named. A project answer does not name one:
   it carries the team as the team's project id. */
static const char *
author_of (nuvashalauncherDownloadModpackDialog *self)
{
  const char *who = self->project != NULL ? self->project->author : NULL;

  if (who == NULL || *who == '\0')
    who = self->details != NULL ? self->details->author : NULL;

  return who != NULL && *who != '\0' ? who : NULL;
}

/* Build the details view after metadata and versions are available. */
GtkWidget *
details_content (nuvashalauncherDownloadModpackDialog *self)
{
  GtkWidget *content = NULL;
  GtkScrolledWindow *scroller = NULL;
  GtkWidget *box = NULL;
  GtkWidget *page = NULL;
  GtkWidget *heading = NULL;
  GtkWidget *bar = NULL;
  g_autoptr (GtkBuilder) body_builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-details-body.ui");
  g_autoptr (GtkBuilder) toolbar_builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-details-toolbar.ui");
  g_autofree char *downloads = NULL;
  const char *who = author_of (self);

  if (self->project == NULL)
    return gtk_label_new ("");

  /* Keep the builder's root alive after the builder is released below. */
  content = GTK_WIDGET (g_object_ref (gtk_builder_get_object (
    body_builder, "content")));
  nuvashalauncher_ui_prepare_dialog_content (content);
  gtk_widget_add_css_class (content, "nuvashalauncher-modpack-details-content");
  scroller = GTK_SCROLLED_WINDOW (gtk_builder_get_object (body_builder, "scroller"));
  {
    GtkWidget *error_label = GTK_WIDGET (gtk_builder_get_object (
      body_builder, "error-label"));
    GtkWidget *parent = gtk_widget_get_parent (error_label);

    /* The template starts the label under @content, but the details page
     * places it below the Markdown body instead. Move it before appending. */
    if (parent != NULL && GTK_IS_BOX (parent))
      gtk_box_remove (GTK_BOX (parent), error_label);
    details_watch (&self->details_error, error_label);
  }
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  heading = adw_preferences_group_new ();

  /* Show the title, author/license, and download count. */
  page = gtk_label_new (self->project != NULL && self->project->title != NULL
                          ? self->project->title
                          : "");
  gtk_label_set_wrap (GTK_LABEL (page), TRUE);
  gtk_label_set_xalign (GTK_LABEL (page), 0);
  gtk_widget_add_css_class (GTK_WIDGET (page), "title-2");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), GTK_WIDGET (page));

  {
    g_autofree char *text = NULL;

    text = nuvashalauncher_content_format_author (
      self->details != NULL ? self->details->license : NULL, who);

    details_watch (&self->meta, gtk_label_new (text));
    gtk_label_set_xalign (GTK_LABEL (self->meta), 0);
    gtk_widget_add_css_class (GTK_WIDGET (self->meta), "dim-label");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), GTK_WIDGET (self->meta));
  }

   downloads = nuvashalauncher_content_format_downloads (self->project->downloads);
  details_watch (&self->count, gtk_label_new (downloads));
  gtk_label_set_xalign (GTK_LABEL (self->count), 0);
  gtk_widget_add_css_class (GTK_WIDGET (self->count), "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), GTK_WIDGET (self->count));

  /* The long description, in Markdown. */
  details_watch (&self->body, gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));

  if (self->details_problem != NULL)
    {
      gtk_box_append (GTK_BOX (self->body), plain_label (self->details_problem));
      const GError *error = g_object_get_data (G_OBJECT (self), "details-error");
      if (error != NULL)
        gtk_box_append (GTK_BOX (self->body), nuvashalauncher_ui_error_details (error));
    }
  else if (self->details != NULL && self->details->body != NULL &&
           *self->details->body != '\0')
    nuvashalauncher_markdown_render (GTK_BOX (self->body), self->details->body);
  else
    gtk_box_append (GTK_BOX (self->body),
                    plain_label (self->project->description));

  gtk_box_append (GTK_BOX (box), GTK_WIDGET (heading));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->body));

  /* Explain missing or failed version lists instead of disabling the button
   * without feedback. */
  gtk_widget_add_css_class (GTK_WIDGET (self->details_error), "dim-label");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->details_error));

  gtk_widget_set_margin_top (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (box), 12);
  gtk_widget_set_margin_start (GTK_WIDGET (box), 18);
  gtk_widget_set_margin_end (GTK_WIDGET (box), 18);

  gtk_scrolled_window_set_child (scroller, GTK_WIDGET (box));

  /* Version selector and download action. */
  bar = GTK_WIDGET (gtk_builder_get_object (toolbar_builder, "toolbar"));
  details_watch (&self->dropdown, GTK_WIDGET (gtk_builder_get_object (toolbar_builder, "dropdown")));
  details_watch (&self->download_button,
                 GTK_WIDGET (gtk_builder_get_object (toolbar_builder, "download-button")));

  gtk_widget_set_sensitive (GTK_WIDGET (self->download_button), FALSE);
  g_signal_connect_object (self->download_button, "clicked",
                           G_CALLBACK (on_modpack_download_clicked), self, 0);

  {
    GtkWidget *parent = gtk_widget_get_parent (bar);

    if (parent != NULL && GTK_IS_BOX (parent))
      gtk_box_remove (GTK_BOX (parent), bar);
    gtk_box_append (GTK_BOX (content), bar);
  }

  return content;
}

/* Puts the real content in the half once both the description and the
   versions are in. */
static void
dialog_details_fill (nuvashalauncherDownloadModpackDialog *self)
{
  if (self->details_page == NULL)
    return;
  if (!self->details_ready || !self->versions_ready)
    return;

   {
     GtkWidget *content = details_content (self);

     adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self->details_page),
                                    content);
     g_object_unref (content);
   }

  if (self->versions_problem != NULL)
    show_details_error (self, self->versions_problem);
  else if (self->versions != NULL && self->versions->len == 0)
    show_details_error (self,
                         _("This project publishes no pack file Nuvasha Launcher can "
                           "install."));

  if (self->versions != NULL && self->versions->len > 0)
    {
      GtkStringList *strings = build_version_strings (self->versions);

      gtk_drop_down_set_model (GTK_DROP_DOWN (self->dropdown),
                               G_LIST_MODEL (strings));
       /* Select the newest version by default. */
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
show_details_error (nuvashalauncherDownloadModpackDialog *self, const char *message)
{
  if (self->details_error == NULL)
    return;

  gtk_label_set_text (GTK_LABEL (self->details_error),
                      message != NULL ? message : "");
  gtk_widget_set_visible (GTK_WIDGET (self->details_error), message != NULL);
  const GError *error = g_object_get_data (G_OBJECT (self), "versions-error");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->details_error),
                               error != NULL ? nuvashalauncher_error_message (error) : NULL);
}

typedef struct
{
  nuvashalauncherDownloadModpackDialog *dialog;
  guint generation;
} VersionsAsk;

static void
versions_ask_free (VersionsAsk *ask)
{
  g_object_unref (ask->dialog);
  g_free (ask);
}

static void
on_versions_answer (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  VersionsAsk *ask = user_data;
  nuvashalauncherDownloadModpackDialog *self = ask->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  parser = nuvashalauncher_download_json_finish (result, &error);

  if (ask->generation != self->details_generation)
    {
      versions_ask_free (ask);
      return;
    }

  if (parser == NULL)
    {
      if (!g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                            nuvashalauncher_DOWNLOAD_ERROR_CANCELLED))
        {
          g_free (self->versions_problem);
          self->versions_problem =
            g_strdup (error != NULL ? nuvashalauncher_error_message (error)
                                    : _("Could not load versions. Try again."));
          g_object_set_data_full (G_OBJECT (self), "versions-error",
                                  error != NULL ? g_error_copy (error) : NULL,
                                  (GDestroyNotify) g_error_free);
        }

      versions_ask_free (ask);
      return;
    }

  versions = versions_of_answer (json_parser_get_root (parser), &error);

  if (versions == NULL)
    {
      g_free (self->versions_problem);
      self->versions_problem =
        g_strdup (nuvashalauncher_error_message (error));
      g_object_set_data_full (G_OBJECT (self), "versions-error",
                              error != NULL ? g_error_copy (error) : NULL,
                              (GDestroyNotify) g_error_free);
      versions_ask_free (ask);
      return;
    }

  g_ptr_array_sort (versions, by_date_desc);

  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = versions;
  versions = NULL;

  self->versions_ready = TRUE;
  dialog_details_fill (self);

  versions_ask_free (ask);
}

static void dialog_details_fill (nuvashalauncherDownloadModpackDialog *self);

/* One answer the dialog asked for and has not released yet: the dialog it
 * belongs to, which can be closed while the request is on its way, and
 * the operation handle the answer is meant to release. */
typedef struct
{
  nuvashalauncherDownloadModpackDialog *dialog;
  nuvashalauncherTaskScope *operation;
  guint generation;
} DetailsAsk;

static void
ask_free (DetailsAsk *ask)
{
  g_clear_object (&ask->dialog);

  if (ask->operation != NULL)
    nuvashalauncher_operation_unref (ask->operation);

  g_free (ask);
}

/* The long description of the pack, read once the dialog is up. */
static void
on_details_answer (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  DetailsAsk *ask = user_data;
  nuvashalauncherDownloadModpackDialog *self = ask->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModrinthDetails) details = NULL;

  details = nuvashalauncher_modrinth_details_finish (result, &error);

  if (ask->generation != self->details_generation)
    {
      ask_free (ask);
      return;
    }

  if (details == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_free (self->details_problem);
          self->details_problem =
            g_strdup (error != NULL ? nuvashalauncher_error_message (error)
                                    : _("Could not load project details. Try again."));
          g_object_set_data_full (G_OBJECT (self), "details-error",
                                  error != NULL ? g_error_copy (error) : NULL,
                                  (GDestroyNotify) g_error_free);
        }
    }
  else
    {
      g_clear_pointer (&self->details, nuvashalauncher_modrinth_details_free);
      self->details = g_steal_pointer (&details);
    }

  self->details_ready = TRUE;
  dialog_details_fill (self);

  ask_free (ask);
}

/* The icon of the project is previewed from the shared icon cache. It is
 * copied into the new instance before that instance points at it: clearing
 * nuvashalauncher's cache must never remove an instance's icon. The reference the load
 * holds keeps the dialog alive until the answer is in. */
typedef struct
{
  nuvashalauncherDownloadModpackDialog *dialog;
  nuvashalauncherTaskScope *operation;
} ProjectIconLoad;

static void
project_icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  ProjectIconLoad *load = user_data;
  nuvashalauncherDownloadModpackDialog *self = load->dialog;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = nuvashalauncher_icon_load_finish (result, &error);

  if (path != NULL)
    {
      g_free (self->icon_path);
      self->icon_path = g_steal_pointer (&path);
    }

  g_clear_object (&load->dialog);
  if (load->operation != NULL)
    nuvashalauncher_operation_unref (load->operation);
  g_free (load);
}

void
dialog_details_open (nuvashalauncherDownloadModpackDialog *self)
{
  g_autofree char *escaped_id = NULL;
  g_autofree char *url = NULL;

  if (self->project == NULL)
    return;

  escaped_id = g_uri_escape_string (self->project->project_id, NULL, FALSE);
  url = g_strdup_printf (nuvashalauncher_MODRINTH_URL "/project/%s/version", escaped_id);

  self->details_ready = FALSE;
  self->versions_ready = FALSE;
  self->details_generation++;
  g_clear_pointer (&self->details, nuvashalauncher_modrinth_details_free);
  g_free (self->details_problem);
  self->details_problem = NULL;
  g_object_set_data (G_OBJECT (self), "details-error", NULL);
  g_object_set_data (G_OBJECT (self), "versions-error", NULL);
  g_free (self->versions_problem);
  self->versions_problem = NULL;

  show_details_page (self, nuvashalauncher_ui_loading_child ());

  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);

  g_clear_object (&self->fetch);
  self->fetch = g_cancellable_new ();

  /* A stale icon of another project has no business on this one. */
  g_clear_pointer (&self->icon_path, g_free);

  if (self->project->icon_url != NULL && *self->project->icon_url != '\0')
    {
      ProjectIconLoad *load = g_new0 (ProjectIconLoad, 1);

      load->dialog = g_object_ref (self);
      load->operation = nuvashalauncher_icon_load_async (self->project->icon_url,
                                             project_icon_ready, load);
      /* The same fetch cancellable the details and versions use, so one
         new project cancels all three. */
      nuvashalauncher_operation_follow (load->operation, self->fetch);
    }

  /* The long description, and the versions of the pack beside it. */
  {
    DetailsAsk *ask = g_new0 (DetailsAsk, 1);

    ask->dialog = g_object_ref (self);
    ask->generation = self->details_generation;
    ask->operation = nuvashalauncher_modrinth_details_async (self->project->project_id,
                                                  on_details_answer, ask);
    /* The same fetch cancellable the versions answer uses, so opening
       another project stops this one. */
    nuvashalauncher_operation_follow (ask->operation, self->fetch);
  }

  {
    VersionsAsk *ask = g_new0 (VersionsAsk, 1);

    ask->dialog = g_object_ref (self);
    ask->generation = self->details_generation;
    nuvashalauncher_operation_release (
      nuvashalauncher_download_json_async (nuvashalauncher_downloads_get_service (), url,
                               on_versions_answer, ask),
      self->fetch);
  }
}
