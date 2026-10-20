/* Content-folder scanning and Modrinth identification. */
#include "nuvasha-content-page-private.h"
#include "nuvasha-content-files-private.h"
#include "nuvasha-content-format.h"
#include "nuvasha-content-index.h"
#include "nuvasha-download.h"
#include "nuvasha-operation.h"
#include "nuvasha-settings.h"
#include "nuvasha-modrinth.h"
#include "nuvasha-i18n.h"
#include <glib/gstdio.h>
#include <string.h>

static void metadata_ready (GObject *source, GAsyncResult *result,
                            gpointer user_data);

/* Ask Modrinth several hashes at once, while keeping the request burst
 * bounded for large content folders. */
#define IDENTIFY_CONCURRENCY 8

typedef struct _IdentifyRun IdentifyRun;
typedef struct {
  IdentifyRun *run;
  NuvashaContentFile *file;
  GtkWidget *row;

  /* The operation that answers for this file, released with the request. */
  NuvashaTaskScope *operation;
} IdentifyRequest;

typedef struct {
  NuvashaContentPage *page; /* weakly held */
  char *project_id;
  NuvashaTaskScope *operation;
} ProjectTitleRequest;

struct _IdentifyRun {
  NuvashaContentPage *page; /* weakly held */
  GCancellable *cancellable;
  guint next_index;
  guint in_flight;
  gboolean finished;
};

static void identify_start (IdentifyRun *run);

static void
identify_run_maybe_free (IdentifyRun *run)
{
  if (!run->finished || run->in_flight != 0)
    return;
  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page),
                                  (gpointer *) &run->page);

  g_clear_object (&run->cancellable);
  g_free (run);
}

static void
identify_complete (IdentifyRun *run)
{
  if (!run->finished)
    {
      run->finished = TRUE;
      if (run->page != NULL)
        {
          run->page->installed.identifying = FALSE;
          if (run->page->project.details_versions != NULL &&
              run->page->project.details_download != NULL)
            {
              gboolean have_versions =
                run->page->project.details_versions->len > 0;
              gtk_widget_set_sensitive (
                GTK_WIDGET (run->page->project.details_download), have_versions);
              gtk_widget_set_sensitive (
                GTK_WIDGET (run->page->project.details_check), have_versions);
            }
        }
    }
  identify_run_maybe_free (run);
}

static void
identify_request_free (IdentifyRequest *request)
{
  IdentifyRun *run = request->run;

  if (request->operation != NULL)
    nuvasha_operation_unref (request->operation);

  g_free (request);
  run->in_flight--;
  identify_run_maybe_free (run);
}

void
persist_identified_file (NuvashaContentPage *self, NuvashaContentFile *file)
{
  g_autofree char *directory = NULL;
  g_autofree char *filename = g_path_get_basename (file->path);
  NuvashaContentIndexEntry entry = { 0 };

  g_object_get (self->instance, "directory", &directory, NULL);
  entry.filename = filename;
  entry.size = file->size;
  entry.mtime_usec = file->mtime_usec;
  entry.hash = file->sha1;
  entry.hash512 = file->sha512;
  entry.download_url = file->download_url;
  entry.env_client = "required";
  entry.env_server = self->type == nuvasha_CONTENT_MOD ? "required"
                                                    : "unsupported";
  entry.provider = "Modrinth";
  entry.project_id = file->project_id;
  entry.project_name = file->project_name;
  entry.icon_url = file->icon_url;
  entry.metadata_checked = file->metadata_checked;
  entry.local_icon_member = file->local_icon_member;
  entry.local_icon_path = file->local_icon_path;
  entry.version_id = file->version_id;
  entry.version_name = file->version_name;
  entry.required_dependencies = file->required_dependencies;
  entry.optional_dependencies = file->optional_dependencies;
  entry.incompatible_dependencies = file->incompatible_dependencies;
  nuvasha_operation_release (
    nuvasha_content_index_update_async (directory, self->type, &entry, NULL, NULL),
    self->installed.scan);
}

static void
project_title_request_free (ProjectTitleRequest *request)
{
  if (request->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (request->page),
                                  (gpointer *) &request->page);
  if (request->operation != NULL)
    nuvasha_operation_unref (request->operation);
  g_free (request->project_id);
  g_free (request);
}

static void
on_project_title_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                         gpointer user_data)
{
  ProjectTitleRequest *request = user_data;
  NuvashaContentPage *self = request->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModrinthDetails) details =
    nuvasha_modrinth_details_finish (result, &error);

  if (self != NULL && details != NULL && details->title != NULL &&
      *details->title != '\0')
    {
      if (self->browse.model == NULL || self->installed.files == NULL)
        {
          project_title_request_free (request);
          return;
        }

      g_hash_table_replace (self->browse.model->project_titles,
                            g_strdup (request->project_id),
                            g_strdup (details->title));

      for (guint i = 0; i < self->installed.files->len; i++)
        {
          NuvashaContentFile *file =
            g_ptr_array_index (self->installed.files, i);

          if (g_strcmp0 (file->project_id, request->project_id) != 0)
            continue;
          if (file->project_name != NULL &&
              *file->project_name != '\0' &&
              g_strcmp0 (file->project_name, file->project_id) != 0)
            continue;

          g_free (file->project_name);
          file->project_name = g_strdup (details->title);
          persist_identified_file (self, file);

          GtkWidget *row = self->installed.rows != NULL
                             ? GTK_WIDGET (g_hash_table_lookup (
                                 self->installed.rows, file->path))
                             : NULL;
          if (row != NULL)
            file_row_refresh (row);
        }
    }

  project_title_request_free (request);
}

static void
request_project_title (NuvashaContentPage *self, const char *project_id)
{
  ProjectTitleRequest *request = NULL;

  if (self == NULL || project_id == NULL || *project_id == '\0')
    return;

  request = g_new0 (ProjectTitleRequest, 1);
  request->page = self;
  request->project_id = g_strdup (project_id);
  g_object_add_weak_pointer (G_OBJECT (self),
                             (gpointer *) &request->page);
  request->operation = nuvasha_modrinth_details_async (
    project_id, on_project_title_ready, request);
  if (request->operation == NULL)
    {
      project_title_request_free (request);
      return;
    }
}

static void
add_remote_dependency (GPtrArray **destination, const char *value)
{
  if (value == NULL)
    return;
  if (*destination == NULL)
    *destination = g_ptr_array_new_with_free_func (g_free);
  for (guint j = 0; j < (*destination)->len; j++)
    if (g_strcmp0 (value, g_ptr_array_index (*destination, j)) == 0)
      return;
  g_ptr_array_add (*destination, g_strdup (value));
}

static void
identify_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  IdentifyRequest *request = user_data;
  IdentifyRun *run = request->run;
  NuvashaContentPage *self = run->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModrinthVersion) version = NULL;
  NuvashaContentFile *file = request->file;
  GtkWidget *row = request->row;

  version = nuvasha_modrinth_version_by_hash_finish (result, &error);

  /* A new folder is being read now: the chain's cancellable is cancelled,
   * so the chain of the old answer stops. */
  if (run->cancellable != NULL &&
      g_cancellable_is_cancelled (run->cancellable))
    {
      run->finished = TRUE;
      identify_request_free (request);
      return;
    }

  if (self == NULL || file == NULL || row == NULL)
    {
      identify_complete (run);
      identify_request_free (request);
      return;
    }

  if (version != NULL)
    {
      file->project_id = g_strdup (version->project_id);
      {
        const char *known_name = g_hash_table_lookup (self->browse.model->project_titles,
                                                      version->project_id);
        if (!file->local_metadata || file->project_name == NULL ||
            *file->project_name == '\0' ||
            g_strcmp0 (file->project_name, version->project_id) == 0)
          {
            g_free (file->project_name);
            file->project_name = g_strdup (known_name);
          }
        if (known_name == NULL)
          request_project_title (self, version->project_id);
      }
      file->version_id = g_strdup (version->version_id);
      file->version_name = g_strdup (version->name);
      file->version_number = g_strdup (version->version_number);
      {
        NuvashaModrinthFile *primary =
          nuvasha_modrinth_version_primary_file (version);

        g_free (file->sha512);
        g_free (file->download_url);
        file->sha512 = primary != NULL ? g_strdup (primary->sha512) : NULL;
        file->download_url = primary != NULL ? g_strdup (primary->url) : NULL;
      }
      for (guint i = 0; version->dependencies != NULL &&
                       i < version->dependencies->len; i++)
        {
          NuvashaModrinthDependency *dependency =
            g_ptr_array_index (version->dependencies, i);

          if (dependency->project_id == NULL)
            continue;
          if (dependency->type == nuvasha_DEPENDENCY_REQUIRED)
            add_remote_dependency (&file->required_dependencies,
                                   dependency->project_id);
          else if (dependency->type == nuvasha_DEPENDENCY_OPTIONAL)
            add_remote_dependency (&file->optional_dependencies,
                                   dependency->project_id);
          else if (dependency->type == nuvasha_DEPENDENCY_INCOMPATIBLE)
            add_remote_dependency (&file->incompatible_dependencies,
                                   dependency->project_id);
        }

      /* Persist the hash lookup immediately. Project details and the icon
       * are a second request; a failure there must not lose the metadata we
       * already obtained from the version-by-hash response. */
      persist_identified_file (self, file);

      if (file->project_id != NULL && *file->project_id != '\0')
        g_hash_table_add (self->browse.model->projects, g_strdup (file->project_id));

      file_row_refresh (row);

      /* The browser's icon of the project the file belongs to, which the
         row carries from here on. */
    }

  /* Modrinth does not know this file, or the network is not there: it
   * keeps its own name and the page works all the same. */
  g_clear_error (&error);

  identify_request_free (request);
  identify_start (run);
}

static void
identify_start (IdentifyRun *run)
{
  NuvashaContentPage *self = run->page;

  if (self == NULL)
    {
      identify_complete (run);
      return;
    }

  while (run->next_index < self->installed.files->len &&
         run->in_flight < IDENTIFY_CONCURRENCY)
    {
      NuvashaContentFile *file =
        g_ptr_array_index (self->installed.files, run->next_index++);
      g_autofree char *key = NULL;
      GtkWidget *row = NULL;

      key = g_strdup (file->path);
      row = g_hash_table_lookup (self->installed.rows, key);

      /* A hash-matched index entry already identifies this file. The row is
       * ready from disk and reopening an instance must not ask Modrinth just
       * to rediscover metadata already stored in the index. */
      if (row == NULL || file->sha1 == NULL)
        continue;

      IdentifyRequest *request = g_new0 (IdentifyRequest, 1);
      request->run = run;
      request->file = file;
      request->row = row;
      run->in_flight++;
      if (file->version_id != NULL && *file->version_id != '\0')
        request->operation =
          nuvasha_modrinth_version_by_id_async (file->version_id, identify_ready,
                                            request);
      else
        request->operation =
          nuvasha_modrinth_version_by_hash_async (file->sha1, identify_ready,
                                              request);

      if (run->cancellable != NULL)
        nuvasha_operation_follow (request->operation,
                                           run->cancellable);
    }

  if (run->next_index >= self->installed.files->len && run->in_flight == 0)
    identify_complete (run);
}

static void
scan_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  PageWatch *watch = user_data;
  NuvashaContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) files = NULL;

  /* The answer is taken even when nobody is left to show it: the result
     belongs to whoever asked, and it is freed by being handed over. */
  files = nuvasha_content_scan_finish (result, &error);

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  if (files == NULL)
    {
      gtk_widget_set_visible (GTK_WIDGET (self->installed.loading), FALSE);
      gtk_widget_set_visible (GTK_WIDGET (self->installed.status), TRUE);
      if (error != NULL && !g_error_matches (error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED))
         show_error (self, _("The folder cannot be read"), error);

      page_watch_free (watch);
      return;
    }

  gtk_widget_set_visible (GTK_WIDGET (self->installed.loading), FALSE);

  /* The rows that pointed into the old list are gone, so the list they
   * pointed into can go too. */
  take_rows_out (&self->installed.rows_list,
                 ADW_PREFERENCES_GROUP (self->installed.group));
  g_clear_pointer (&self->installed.files, g_ptr_array_unref);
  g_clear_pointer (&self->installed.rows, g_hash_table_unref);
  self->installed.files = g_steal_pointer (&files);
  self->installed.rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      NuvashaContentFile *file = g_ptr_array_index (self->installed.files, i);

      if (file->project_id != NULL && file->project_name != NULL &&
          *file->project_name != '\0')
        g_hash_table_replace (self->browse.model->project_titles,
                              g_strdup (file->project_id),
                              g_strdup (file->project_name));
      GtkWidget *row = file_row (self, file);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->installed.group),
                                  row);
      self->installed.rows_list = g_list_append (self->installed.rows_list, row);
      g_hash_table_insert (self->installed.rows, g_strdup (file->path), row);
    }

  installed_filter_rows (self);

  {
    gboolean empty = self->installed.files->len == 0;

    gtk_widget_set_visible (GTK_WIDGET (self->installed.status), empty);
    gtk_widget_set_visible (GTK_WIDGET (self->installed.scroller), !empty);
    gtk_widget_set_sensitive (GTK_WIDGET (self->installed.update_all), !empty);
    if (!empty)
      load_visible_icons (self);
  }

  {
    IdentifyRun *run = g_new0 (IdentifyRun, 1);

    self->installed.identifying = TRUE;
    run->page = self;
    g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
    run->cancellable = g_object_ref (self->installed.scan);
    identify_start (run);
  }

  nuvasha_operation_release (
    nuvasha_content_metadata_async (self->installed.files, self->type,
                                metadata_ready, g_object_ref (self)),
    self->installed.scan);

  page_watch_free (watch);
}

static void
metadata_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  NuvashaContentPage *self = nuvasha_CONTENT_PAGE (user_data);
  g_autoptr (GError) error = NULL;

  if (nuvasha_content_metadata_finish (result, &error))
    {
      for (guint i = 0; self->installed.files != NULL &&
                        i < self->installed.files->len; i++)
        {
           NuvashaContentFile *file =
             g_ptr_array_index (self->installed.files, i);
          GtkWidget *row = GTK_WIDGET (
            g_hash_table_lookup (self->installed.rows, file->path));

           if (row != NULL)
             file_row_refresh (row);

           if (file->metadata_dirty)
             {
               persist_identified_file (self, file);
               file->metadata_dirty = FALSE;
             }
        }
      installed_filter_rows (self);
    }

  g_object_unref (self);
}

void
reload_installed (NuvashaContentPage *self)
{
  g_autofree char *directory = NULL;
  GCancellable *previous = self->installed.scan;

  if (self->installed.scan == NULL || self->installed.group == NULL)
    return;

  self->installed.loaded = TRUE;

  g_object_get (self->instance, "directory", &directory, NULL);

  if (directory == NULL)
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->installed.status), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->installed.scroller), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->installed.loading), TRUE);

  /* Cancel scans for the previous directory contents. */
  self->installed.scan = g_cancellable_new ();
  g_cancellable_cancel (previous);
  g_clear_object (&previous);

  {
    PageWatch *watch = page_watch_new (self);

    watch->operation = nuvasha_content_scan_async (directory, self->type,
                                               scan_ready, watch);
    nuvasha_operation_follow (watch->operation,
                                       self->installed.scan);
  }
}
