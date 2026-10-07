/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlContentPage: the mods, the resource packs or the shader packs of
 * one instance.
 *
 * The content type is the only thing that makes the three pages
 * different: which folder of the instance they read, whether a loader
 * decides what fits, which extensions a file has, and the words and the
 * icon that name them. Everything else here is written once.
 *
 * Installed is read from the folder rather than from Modrinth, so it
 * works with no network at all; the only thing it asks for is the name
 * Modrinth knows a file by, and a file Modrinth does not know is listed
 * under its own name. Browse is Modrinth, narrowed to the Minecraft
 * version and the loader of the instance, so a version that does not fit
 * is never offered and never has to be refused afterwards.
 *
 * Files are written by the download module, which hashes what arrives
 * and only then renames it into place, and the dependencies a version
 * requires are resolved and shown before anything is downloaded.
 *
 * Every request here answers on the main loop with the page only weakly
 * held: a page can be destroyed while a request of its own is still
 * running, so each of them checks that the page is still there before
 * touching it. */

#include "aml-content-page.h"

#include "aml-download.h"
#include "aml-icons.h"

#include <glib/gstdio.h>
#include <string.h>

/* Results on a page of Browse. */
#define RESULTS_PER_PAGE 20

/* The most rows the list keeps. A search that matches thousands of
 * projects would otherwise grow the widget tree without end, and each
 * row is a handful of widgets. The oldest rows go first: the user who
 * scrolled to the bottom of a long list is reading the newest, and the
 * ones above are the ones a new search would have replaced anyway. */
#define MAX_RESULT_ROWS 400
#define RESULT_ROWS_TRIM 200

/* How long the words of a search entry rest before they are asked for,
 * so that typing is not one request per letter. Long enough to be past a
 * fast typist, short enough that the list is not forgotten. */
#define SEARCH_DELAY_MS 300

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
  GtkWidget *stack;

  /* The row that decides between Installed and Browse: the actions of
   * the list being shown on the left, the switcher on the right. Only
   * one of the two action boxes is a child of it at a time. */
  GtkWidget *bar;
  GtkWidget *bar_spacer; /* takes the room between the actions and the
                            switcher, so the switcher sits at the right */
  GtkWidget *switcher;
  GtkWidget *installed_actions;
  GtkWidget *browse_actions;

  GtkWidget *installed_group;
  GtkWidget *installed_scroller;
  GtkWidget *installed_status;
  GtkWidget *update_all;
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

  /* What the folder holds, and the row of each file. The rows point
   * into these, so both are only replaced once the rows that point at
   * them are gone. */
  GPtrArray *files;     /* AmlContentFile* */
  GHashTable *rows;     /* path -> row */
  GList *installed_rows; /* the rows of the Installed group */
  GList *result_rows;    /* the rows of the Browse group */
  GList *detail_rows;    /* the rows of the page of a project */
  GHashTable *projects; /* project id -> NULL */

  /* The search: what was asked for, what came back, and where the list
   * stands. */
  char *query;
  GPtrArray *categories; /* char*, what the filter has on */
  GPtrArray *results;    /* AmlModrinthProject* the rows point at */
  guint offset;
  guint64 total_hits;
  gboolean categories_loaded;
  gboolean browse_searched;

  /* The infinite scroll: whether a page is on its way, the projects
   * already listed (so a page is never added twice) and the cancellable
   * of the page being fetched. */
  gboolean loading_page;
  gboolean pending_append;
  GHashTable *seen_ids; /* project id -> NULL */
  GCancellable *more;   /* the next page, cancelled by a new search */

  /* The page of a project that was pushed above this one, and what it
   * asked for. */
  AdwPreferencesGroup *details_versions;
  char *details_project;

  guint debounce_source;
  GCancellable *browse;   /* searching and the categories of the filter */
  GCancellable *scan;     /* the folder, its hashes and their lookups */
  GCancellable *transfer; /* what is being installed or updated */
};

G_DEFINE_FINAL_TYPE (AmlContentPage, aml_content_page, GTK_TYPE_BOX)

/* The update of every file Modrinth knows, one after the other. */
typedef struct {
  AmlContentPage *page; /* weakly held */
  guint index;
} UpdateRun;

static void run_search (AmlContentPage *self, gboolean append);
static void reload_installed (AmlContentPage *self);
static void update_run_next (UpdateRun *run);

/* One answer of an asynchronous request that only reads the page. The
 * page is weakly held: a page can be closed while a search, a scan or a
 * version lookup of its own is still running, and the answer must then
 * do nothing rather than touch memory that is gone. */
typedef struct {
  AmlContentPage *page; /* weak */
} PageWatch;

static PageWatch *
page_watch_new (AmlContentPage *self)
{
  PageWatch *watch = g_new0 (PageWatch, 1);

  watch->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &watch->page);

  return watch;
}

static void
page_watch_free (PageWatch *watch)
{
  if (watch->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (watch->page),
                                  (gpointer *) &watch->page);

  g_free (watch);
}

/* ---------------------------------------------------------- *
 * Small helpers
 * ---------------------------------------------------------- */

static char *
instance_string (AmlInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

/* A count of downloads in the fewest words that still say what it is. */
static char *
format_downloads (guint64 downloads)
{
  if (downloads >= 1000000)
    return g_strdup_printf ("%.1fM downloads", downloads / 1000000.0);
  if (downloads >= 1000)
    return g_strdup_printf ("%.1fk downloads", downloads / 1000.0);

  return g_strdup_printf ("%" G_GUINT64_FORMAT " downloads", downloads);
}

static void
show_toast (AmlContentPage *self, const char *message)
{
  AdwToast *toast = NULL;

  if (self->toasts == NULL)
    return;

  toast = adw_toast_new (message);
  /* Messages come from GError: plain text, never markup. */
  adw_toast_set_use_markup (toast, FALSE);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toasts), toast);
}

static void
show_error (AmlContentPage *self, const char *what, GError *error)
{
  g_autofree char *message = g_strdup_printf (
      "%s: %s", what,
      error != NULL ? error->message : "something went wrong");

  show_toast (self, message);
}

/* A spinner says that something is on its way, and nothing about how
 * long it will take. */
static void
set_busy (AmlContentPage *self, gboolean busy)
{
  if (self->spinner != NULL)
    gtk_widget_set_visible (self->spinner, busy);
}

/* One flat button with a symbolic icon, the way GNOME writes a toolbar
 * action: the label is the tooltip and the accessible name. */
static GtkWidget *
toolbar_button (const char *icon_name, const char *label, GCallback callback,
                gpointer user_data)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon_name);

  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (button, label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect (button, "clicked", callback, user_data);

  return button;
}

/* Takes the rows a group was given back out of it. The rows are the ones
 * that were added, because the children of a group are the widgets
 * inside it. */
static void
take_rows_out (GList **rows, AdwPreferencesGroup *group)
{
  if (group == NULL)
    {
      g_list_free (*rows);
      *rows = NULL;
      return;
    }

  for (GList *item = *rows; item != NULL; item = item->next)
    adw_preferences_group_remove (group, item->data);

  g_list_free (*rows);
  *rows = NULL;
}

/* ---------------------------------------------------------- *
 * The files of the instance folder
 * ---------------------------------------------------------- */

void
aml_content_file_free (AmlContentFile *file)
{
  if (file == NULL)
    return;

  g_free (file->path);
  g_free (file->filename);
  g_free (file->sha1);
  g_free (file->project_id);
  g_free (file->version_name);
  g_free (file->version_number);
  g_free (file);
}

char *
aml_content_folder_path (const char *instance_directory, AmlContentType type)
{
  g_return_val_if_fail (instance_directory != NULL, NULL);

  return g_build_filename (instance_directory,
                           aml_content_type_folder (type), NULL);
}

char *
aml_content_disabled_name (const char *filename, gboolean enabled)
{
  const char *suffix = AML_CONTENT_DISABLED_SUFFIX;
  gsize length = 0;

  g_return_val_if_fail (filename != NULL, NULL);

  length = strlen (filename);

  if (enabled)
    {
      if (length > strlen (suffix) && g_str_has_suffix (filename, suffix))
        return g_strndup (filename, length - strlen (suffix));

      return g_strdup (filename);
    }

  if (g_str_has_suffix (filename, suffix))
    return g_strdup (filename);

  return g_strconcat (filename, suffix, NULL);
}

char *
aml_content_set_disabled (const char *path, gboolean disabled, GError **error)
{
  g_autofree char *directory = NULL;
  g_autofree char *base = NULL;
  g_autofree char *name = NULL;
  g_autofree char *target = NULL;
  g_autoptr (GFile) from = NULL;
  g_autoptr (GFile) to = NULL;

  g_return_val_if_fail (path != NULL, NULL);

  directory = g_path_get_dirname (path);
  base = g_path_get_basename (path);
  name = aml_content_disabled_name (base, disabled);
  target = g_build_filename (directory, name, NULL);

  if (g_strcmp0 (path, target) == 0)
    return g_steal_pointer (&target);

  from = g_file_new_for_path (path);
  to = g_file_new_for_path (target);

  /* A rename of a local file is one system call: it waits for nothing
   * and moves no byte of the file itself. */
  if (!g_file_move (from, to, G_FILE_COPY_NONE, NULL, NULL, NULL, error))
    return NULL;

  return g_steal_pointer (&target);
}

/* TRUE when @name is a file of this content, suffix or not: the loaders
 * and the game only pick up the ones that do not carry the suffix. */
static gboolean
name_is_content (const char *name, const char *extension)
{
  g_autofree char *plain = aml_content_disabled_name (name, TRUE);

  return g_str_has_suffix (plain, extension);
}

/* The scan: what the folder holds and the hash of each file, which is
 * what Modrinth is then asked about. */
typedef struct
{
  GTask *task;
  GFile *folder;
  GFileEnumerator *enumerator; /* while the batches are read */
  char *extension;
  GPtrArray *files; /* AmlContentFile* */
  guint index;
} ScanRun;

static void scan_hash_next (ScanRun *run);
static void batch_read_next (ScanRun *run);
static int compare_files (gconstpointer a, gconstpointer b);

static void
scan_free (ScanRun *run)
{
  g_clear_object (&run->enumerator);
  g_clear_object (&run->folder);
  g_clear_object (&run->task);
  g_clear_pointer (&run->files, g_ptr_array_unref);
  g_free (run->extension);
  g_free (run);
}

/* One batch of the folder, then the next, so that a folder with a
 * thousand files in it is read without the main loop waiting. */
static void
batch_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autolist (GFileInfo) children = NULL;

  children = g_file_enumerator_next_files_finish (run->enumerator, result,
                                                  &error);

  if (g_task_return_error_if_cancelled (run->task))
    {
      scan_free (run);
      return;
    }

  /* No list and no error is how an enumeration says it has read the
   * whole folder. */
  if (children == NULL && error != NULL)
    {
      g_task_return_error (run->task, g_steal_pointer (&error));
      scan_free (run);
      return;
    }

  for (GList *item = children; item != NULL; item = item->next)
    {
      GFileInfo *info = item->data;
      /* Borrowed from @info, which is dropped with the list. */
      const char *name = g_file_info_get_name (info);
      g_autoptr (GFile) child = NULL;
      AmlContentFile *file = NULL;

      if (name == NULL ||
          g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
        continue;

      if (!name_is_content (name, run->extension))
        continue;

      child = g_file_get_child (run->folder, name);
      file = g_new0 (AmlContentFile, 1);
      file->path = g_file_get_path (child);
      file->filename = aml_content_disabled_name (name, TRUE);
      file->enabled = g_strcmp0 (name, file->filename) == 0;
      g_ptr_array_add (run->files, file);
    }

  if (children != NULL)
    {
      batch_read_next (run);
      return;
    }

  scan_hash_next (run);
}

static void
batch_read_next (ScanRun *run)
{
  g_file_enumerator_next_files_async (run->enumerator, 64, G_PRIORITY_DEFAULT,
                                      g_task_get_cancellable (run->task),
                                      batch_ready, run);
}

static void
scan_children_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileEnumerator) enumerator = NULL;

  enumerator = g_file_enumerate_children_finish (G_FILE (source), result,
                                                 &error);

  if (enumerator == NULL)
    {
      g_task_return_error (run->task, g_steal_pointer (&error));
      scan_free (run);
      return;
    }

  run->enumerator = g_steal_pointer (&enumerator);
  batch_read_next (run);
}

static void
scan_hash_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *digest = NULL;
  AmlContentFile *file = g_ptr_array_index (run->files, run->index);

  digest = aml_download_file_hash_finish (result, &error);

  if (g_task_return_error_if_cancelled (run->task))
    {
      scan_free (run);
      return;
    }

  if (digest != NULL)
    {
      file->sha1 = g_steal_pointer (&digest);
    }
  else
    {
      /* A file that cannot be read is still a file that is there: it is
       * listed under its own name, without an answer from Modrinth. */
      g_clear_error (&error);
    }

  run->index++;
  scan_hash_next (run);
}

static void
scan_hash_next (ScanRun *run)
{
  AmlContentFile *file = NULL;
  g_autoptr (GFile) handle = NULL;

  if (run->index >= run->files->len)
    {
      g_ptr_array_sort (run->files, compare_files);
      g_task_return_pointer (run->task, g_steal_pointer (&run->files),
                             (GDestroyNotify) g_ptr_array_unref);
      scan_free (run);
      return;
    }

  file = g_ptr_array_index (run->files, run->index);
  handle = g_file_new_for_path (file->path);
  aml_download_file_hash_async (handle, G_CHECKSUM_SHA1,
                                g_task_get_cancellable (run->task),
                                scan_hash_ready, run);
}

static int
compare_files (gconstpointer a, gconstpointer b)
{
  const AmlContentFile *left = *(const AmlContentFile *const *) a;
  const AmlContentFile *right = *(const AmlContentFile *const *) b;

  return g_utf8_collate (left->filename, right->filename);
}

void
aml_content_scan_async (const char *instance_directory, AmlContentType type,
                        GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  ScanRun *run = NULL;
  g_autofree char *folder = NULL;

  g_return_if_fail (instance_directory != NULL);

  folder = aml_content_folder_path (instance_directory, type);
  run = g_new0 (ScanRun, 1);
  run->task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_check_cancellable (run->task, FALSE);
  run->folder = g_file_new_for_path (folder);
  run->extension = g_strdup (aml_content_type_extension (type));
  run->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_content_file_free);

  /* An instance that never installed anything of this kind has no such
   * folder, which reads the same as an empty one. */
  if (!g_file_query_exists (run->folder, NULL))
    {
      g_task_return_pointer (run->task, g_steal_pointer (&run->files),
                             (GDestroyNotify) g_ptr_array_unref);
      scan_free (run);
      return;
    }

  g_file_enumerate_children_async (
      run->folder,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, cancellable,
      scan_children_ready, run);
}

GPtrArray *
aml_content_scan_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Writing files
 * ---------------------------------------------------------- */

/* One file of an install: where it comes from, what it is called, and
 * the file it takes the place of, which is what an update leaves
 * behind. */
typedef struct {
  char *url;
  char *filename;
  char *sha1;
  char *label;
  char *replaces;
} InstallFile;

static void
install_file_free (InstallFile *file)
{
  if (file == NULL)
    return;

  g_free (file->url);
  g_free (file->filename);
  g_free (file->sha1);
  g_free (file->label);
  g_free (file->replaces);
  g_free (file);
}

/* The file of @version, named as it has to land in the folder. */
static InstallFile *
install_file_of (AmlModrinthVersion *version, const char *replaces,
                 gboolean disabled)
{
  AmlModrinthFile *file = aml_modrinth_version_primary_file (version);
  InstallFile *install = NULL;
  g_autofree char *name = NULL;
  g_autofree char *old_name = NULL;

  if (file == NULL)
    return NULL;

  install = g_new0 (InstallFile, 1);
  install->url = g_strdup (file->url);
  install->sha1 = g_strdup (file->sha1);
  install->label = g_strdup (version->name);

  name = disabled ? aml_content_disabled_name (file->filename, FALSE)
                  : g_strdup (file->filename);
  install->filename = g_steal_pointer (&name);

  /* An update takes the place of the file it updates, unless it lands
   * under the very same name, in which case it has already replaced it
   * and there is nothing left to remove. */
  if (replaces != NULL)
    {
      old_name = g_path_get_basename (replaces);

      if (g_strcmp0 (old_name, install->filename) != 0)
        install->replaces = g_strdup (replaces);
    }

  return install;
}

/* A queue of files written one after the other, so that a big file never
 * sits in front of the one that needs it and a failure stops where it
 * happened. The page is only weakly held: it can go away while the last
 * file of it is still on its way. */
typedef struct {
  AmlContentPage *page;
  GPtrArray *files; /* InstallFile* */
  guint index;
  GCancellable *cancellable;
} FetchRun;

static void fetch_next (FetchRun *run);

static void
fetch_free (FetchRun *run)
{
  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page), (gpointer *) &run->page);

  g_clear_pointer (&run->files, g_ptr_array_unref);
  g_clear_object (&run->cancellable);
  g_free (run);
}

static void
fetch_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  FetchRun *run = user_data;
  g_autoptr (GError) error = NULL;
  gboolean ok = aml_download_file_finish (result, &error);
  InstallFile *file = NULL;

  if (run->index > 0)
    file = g_ptr_array_index (run->files, run->index - 1);

  /* A download that fails or is cancelled wrote nothing: a file only
   * reaches the folder complete and verified. */
  if (!ok)
    {
      if (run->page != NULL)
        {
          set_busy (run->page, FALSE);
          show_error (run->page, "The file could not be installed", error);
        }

      g_clear_error (&error);
      fetch_free (run);
      return;
    }

  /* The new version is in place, so the file it replaces can go. */
  if (file != NULL && file->replaces != NULL)
    {
      g_autoptr (GFile) old = g_file_new_for_path (file->replaces);

      if (!g_file_delete (old, NULL, NULL))
        g_warning ("the file \"%s\" it replaces could not be removed",
                   file->replaces);
    }

  fetch_next (run);
}

static void
fetch_next (FetchRun *run)
{
  AmlContentPage *self = run->page;
  InstallFile *file = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GFile) folder = NULL;

  if (run->index >= run->files->len)
    {
      if (self != NULL)
        {
          set_busy (self, FALSE);
          reload_installed (self);
        }

      fetch_free (run);
      return;
    }

  file = g_ptr_array_index (run->files, run->index);
  folder = g_file_new_for_path (self->folder);

  {
    g_autoptr (GError) error = NULL;

    if (!g_file_make_directory_with_parents (folder, NULL, &error) &&
        !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
      {
        set_busy (self, FALSE);
        show_error (self, "The folder cannot be created", error);
        fetch_free (run);
        return;
      }
  }

  path = g_build_filename (self->folder, file->filename, NULL);
  run->index++;

  aml_download_file_async (file->url, path, file->sha1, G_CHECKSUM_SHA1,
                           run->cancellable, NULL, fetch_ready, run);
}

/* Takes the files of an install and writes them in order. The run owns
 * @files from here. */
static void
start_fetch (AmlContentPage *self, GPtrArray *files)
{
  FetchRun *run = g_new0 (FetchRun, 1);

  run->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
  run->files = files;
  run->cancellable = g_object_ref (self->transfer);

  fetch_next (run);
}

/* ---------------------------------------------------------- *
 * The plan of an install
 *
 * A version can require other projects, which can require others. The
 * plan walks that with one queue, resolves every project to the newest
 * version that fits the instance, and shows the user the whole list
 * before anything is downloaded.
 * ---------------------------------------------------------- */

typedef struct {
  AmlContentPage *page;        /* weakly held */
  AmlModrinthVersion *version; /* transfer full, the version asked for */
  char *title;
  char *replaces;             /* the file an update takes the place of */
  gboolean stays_disabled;    /* an update of a disabled file stays so */
  GPtrArray *queue;           /* AmlModrinthDependency* to resolve */
  GPtrArray *files;           /* InstallFile* collected, dependencies */
  GHashTable *seen;           /* project ids already in the plan */
  GCancellable *cancellable;
} PlanRun;

static void plan_free (PlanRun *plan);
static void show_plan (AmlContentPage *self, PlanRun *plan);
static void plan_versions_ready (GObject *source, GAsyncResult *result,
                                gpointer user_data);

static void
plan_next (PlanRun *plan)
{
  AmlModrinthDependency *dependency = NULL;
  AmlContentPage *self = plan->page;

  if (self == NULL)
    {
      plan_free (plan);
      return;
    }

  if (plan->queue->len == 0)
    {
      /* Everything the version needs is resolved, so the version itself
       * comes last: the dependencies it is fetched with are the ones
       * that have to be in the folder first. */
      InstallFile *own = install_file_of (plan->version, plan->replaces,
                                          plan->stays_disabled);

      if (own != NULL)
        g_ptr_array_add (plan->files, own);

      show_plan (self, plan);
      return;
    }

  dependency = g_ptr_array_index (plan->queue, 0);
  g_ptr_array_remove_index (plan->queue, 0);

  aml_modrinth_versions_async (dependency->project_id, self->type,
                               self->minecraft_version, self->loader,
                               plan->cancellable, plan_versions_ready, plan);
}

static void
plan_free (PlanRun *plan)
{
  if (plan == NULL)
    return;

  if (plan->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (plan->page),
                                  (gpointer *) &plan->page);

  g_clear_pointer (&plan->version, aml_modrinth_version_free);
  g_clear_pointer (&plan->queue, g_ptr_array_unref);
  g_clear_pointer (&plan->files, g_ptr_array_unref);
  g_clear_pointer (&plan->seen, g_hash_table_unref);
  g_clear_object (&plan->cancellable);
  g_free (plan->title);
  g_free (plan->replaces);
  g_free (plan);
}

static gboolean
plan_has_conflicts (PlanRun *plan)
{
  if (plan->version == NULL || plan->version->dependencies == NULL)
    return FALSE;

  for (guint i = 0; i < plan->version->dependencies->len; i++)
    {
      AmlModrinthDependency *dependency =
        g_ptr_array_index (plan->version->dependencies, i);

      if (dependency->type == AML_DEPENDENCY_INCOMPATIBLE)
        return TRUE;
    }

  return FALSE;
}

/* What the user is asked to confirm: every file that will be written,
 * with any conflict said in as many words. */
static char *
plan_text (PlanRun *plan)
{
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) notes = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *files_text = NULL;
  const char *version = plan->version->version_number[0] != '\0'
                          ? plan->version->version_number
                          : plan->version->name;

  g_ptr_array_add (lines, g_strdup_printf ("%s %s", plan->title, version));

  for (guint i = 0; i < plan->files->len; i++)
    g_ptr_array_add (
        lines, g_strdup (((InstallFile *) g_ptr_array_index (plan->files, i))
                             ->label));

  if (plan->version->dependencies != NULL)
    {
      for (guint i = 0; i < plan->version->dependencies->len; i++)
        {
          AmlModrinthDependency *dependency =
            g_ptr_array_index (plan->version->dependencies, i);

          if (dependency->type != AML_DEPENDENCY_INCOMPATIBLE)
            continue;

          g_ptr_array_add (
              notes,
              g_strdup_printf (
                  "It does not run with %s, which this instance already has.",
                  dependency->project_id != NULL ? dependency->project_id
                                                  : "another file"));
        }
    }

  /* GLib's g_strjoinv wants a NULL-terminated char**; GPtrArray's pdata
   * is not one, so the sentinel is added explicitly. */
  g_ptr_array_add (lines, NULL);
  files_text = g_strjoinv ("\n", (char **) lines->pdata);

  if (notes->len == 0)
    return g_steal_pointer (&files_text);

  {
    g_ptr_array_add (notes, NULL);
    g_autofree char *notes_text =
      g_strjoinv ("\n", (char **) notes->pdata);

    return g_strdup_printf ("%s\n\n%s", files_text, notes_text);
  }
}

static void
on_plan_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlContentPage *self = user_data;
  PlanRun *plan = g_object_get_data (G_OBJECT (source), "aml-plan");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (plan == NULL)
    return;

  if (g_strcmp0 (response, "install") != 0 || plan->page == NULL)
    {
      plan_free (plan);
      return;
    }

  set_busy (self, TRUE);

  {
    g_autofree char *message =
      g_strdup_printf ("%s is being installed", plan->title);

    show_toast (self, message);
  }

  start_fetch (self, plan->files);
  plan->files = NULL; /* the fetch owns them now */
  plan_free (plan);
}

static void
show_plan (AmlContentPage *self, PlanRun *plan)
{
  g_autofree char *heading = g_strdup_printf (
      "%s \"%s\"?", plan->replaces != NULL ? "Update" : "Install",
      plan->title);
  g_autofree char *body = plan_text (plan);
  AdwAlertDialog *alert =
    ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", "Cancel");
  adw_alert_dialog_add_response (
      alert, "install",
      plan_has_conflicts (plan) ? "Install Anyway" : "Install");
  adw_alert_dialog_set_response_appearance (alert, "install",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  g_object_set_data (G_OBJECT (alert), "aml-plan", plan);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_plan_chosen,
                           g_object_ref (self));
}

/* What @version needs that the plan does not have yet. A dependency
 * that names nothing to fetch stops the plan instead of writing half of
 * it. */
static gboolean
plan_take (PlanRun *plan, AmlModrinthVersion *version)
{
  AmlContentPage *self = plan->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) pending = NULL;

  pending = aml_modrinth_resolve_install (version, NULL, NULL, &error);

  if (pending == NULL)
    {
      if (self != NULL)
        {
          show_error (self, "This cannot be installed", error);
          set_busy (self, FALSE);
        }

      return FALSE;
    }

  for (guint i = 0; i < pending->len; i++)
    {
      AmlModrinthDependency *dependency = g_ptr_array_index (pending, i);

      /* One copy of a project is one file: the second time round it is
       * already in the plan. */
      if (g_hash_table_contains (plan->seen, dependency->project_id))
        continue;

      g_hash_table_add (plan->seen, g_strdup (dependency->project_id));
      g_ptr_array_add (plan->queue,
                       aml_modrinth_dependency_copy (dependency));
    }

  return TRUE;
}

static void
plan_versions_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  PlanRun *plan = user_data;
  AmlContentPage *self = plan->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  AmlModrinthVersion *version = NULL;
  InstallFile *file = NULL;

  versions = aml_modrinth_versions_finish (result, &error);

  if (versions == NULL)
    {
      /* What a project publishes for nothing that fits is said in the
       * words the page shows everywhere else. */
      if (self != NULL)
        {
          show_toast (self, error != NULL ? error->message
                                          : "no compatible version");
          set_busy (self, FALSE);
        }

      plan_free (plan);
      return;
    }

  /* The list is newest first, so the first is what an install takes. */
  version = g_ptr_array_steal_index (versions, 0);
  file = install_file_of (version, NULL, FALSE);

  if (file != NULL)
    g_ptr_array_add (plan->files, file);

  if (!plan_take (plan, version))
    {
      aml_modrinth_version_free (version);
      plan_free (plan);
      return;
    }

  aml_modrinth_version_free (version);
  plan_next (plan);
}

/* The install of one version and of everything it requires. */
static void
start_plan (AmlContentPage *self, AmlModrinthVersion *version,
            const char *title, const char *replaces, gboolean disabled)
{
  PlanRun *plan = g_new0 (PlanRun, 1);

  plan->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &plan->page);
  plan->version = version; /* the plan owns it from here */
  plan->title = g_strdup (title);
  plan->replaces = replaces != NULL ? g_strdup (replaces) : NULL;
  plan->stays_disabled = disabled;
  plan->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) install_file_free);
  plan->queue = g_ptr_array_new_with_free_func (
      (GDestroyNotify) aml_modrinth_dependency_free);
  plan->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  plan->cancellable = g_object_ref (self->browse);

  if (!plan_take (plan, version))
    {
      plan_free (plan);
      return;
    }

  /* Show the plan first: the file itself is listed in it, and only the
   * fetch writes anything to the folder. */
  plan_next (plan);
}

/* The version a project publishes that fits the instance. */
static void
on_pick_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  const char *title = NULL;
  AmlModrinthVersion *version = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  versions = aml_modrinth_versions_finish (result, &error);

  if (versions == NULL)
    {
      set_busy (self, FALSE);
      show_toast (self, error != NULL ? error->message
                                      : "no compatible version");
      page_watch_free (watch);
      return;
    }

  title = g_object_get_data (G_OBJECT (self), "aml-pick-title");
  version = g_ptr_array_steal_index (versions, 0);
  start_plan (self, version, title != NULL ? title : "the file", NULL, FALSE);
  set_busy (self, FALSE);
  page_watch_free (watch);
}

/* ---------------------------------------------------------- *
 * Installed: the rows and what can be done to them
 * ---------------------------------------------------------- */

static void on_remove_chosen (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void on_update_clicked (GtkButton *button, gpointer user_data);

static void
toggle_file (AmlContentPage *self, AmlContentFile *file)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *renamed = NULL;

  renamed = aml_content_set_disabled (file->path, !file->enabled, &error);

  if (renamed == NULL)
    {
      show_error (self, "The file cannot be renamed", error);
      return;
    }

  show_toast (self, file->enabled ? "Disabled" : "Enabled");
  reload_installed (self);
}

static void
remove_file (AmlContentPage *self, AmlContentFile *file)
{
  g_autofree char *heading =
    g_strdup_printf ("Remove \"%s\"?", file->filename);
  g_autofree char *body =
    g_strdup_printf ("\"%s\" is deleted from this instance.", file->path);
  AdwAlertDialog *alert =
    ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", "Cancel");
  adw_alert_dialog_add_response (alert, "remove", "Remove");
  adw_alert_dialog_set_response_appearance (alert, "remove",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  g_object_set_data (G_OBJECT (alert), "aml-file", file);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_remove_chosen,
                           g_object_ref (self));
}

static void
file_row_refresh (GtkWidget *row)
{
  AmlContentFile *file = g_object_get_data (G_OBJECT (row), "aml-file");
  GtkWidget *update = g_object_get_data (G_OBJECT (row), "aml-update");

  if (file == NULL || !GTK_IS_WIDGET (row))
    return;

  /* What Modrinth knows the file by, or the name it has on disk when
   * Modrinth does not know it. */
  adw_preferences_row_set_title (
      ADW_PREFERENCES_ROW (row),
      file->version_name != NULL ? file->version_name : file->filename);

  /* Only a file Modrinth knows can be asked what is newer. */
  gtk_widget_set_sensitive (update, file->project_id != NULL);
}

/* Two things can be done to one installed file: stop loading it, or
 * take it out of the instance. Each button of the popover carries the
 * file it is about, so the row does not have to be looked up. */
static void
on_toggle_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlContentFile *file = g_object_get_data (G_OBJECT (button), "aml-file");

  if (file != NULL)
    toggle_file (self, file);
}

static void
on_remove_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlContentFile *file = g_object_get_data (G_OBJECT (button), "aml-file");

  if (file != NULL)
    remove_file (self, file);
}

static GtkWidget *
file_row (AmlContentPage *self, AmlContentFile *file)
{
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *icon = aml_icon_widget_new (aml_content_type_icon (self->type));
  GtkWidget *suffix = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *update = gtk_button_new_with_label ("Update");
  GtkWidget *menu_button = gtk_menu_button_new ();
  GtkWidget *icon_more = gtk_image_new_from_icon_name ("view-more-symbolic");
  GtkWidget *popover = gtk_popover_new ();
  GtkWidget *items = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *toggle = gtk_button_new_with_label (file->enabled ? "Disable"
                                                               : "Enable");
  GtkWidget *remove = gtk_button_new_with_label ("Remove");

  /* A file name and a path are plain text: markup off first, because
     either may hold an "&". */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), file->filename);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), file->path);

  /* The same square as a browse row, with the icon of what this kind of
     file is: a file on disk has no icon of its own to fetch. */
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);

  gtk_widget_add_css_class (update, "flat");
  gtk_widget_set_valign (update, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (update, "Look for a newer version");
  g_object_set_data (G_OBJECT (update), "aml-file", file);

  gtk_image_set_pixel_size (GTK_IMAGE (icon_more), 16);
  gtk_widget_set_valign (menu_button, GTK_ALIGN_CENTER);
  gtk_menu_button_set_child (GTK_MENU_BUTTON (menu_button), icon_more);

  /* The button is only the three dot icon; the row title says which
     file, but the button needs a name of its own to be announced. */
  gtk_widget_set_tooltip_text (menu_button, "More actions");
  gtk_accessible_update_property (GTK_ACCESSIBLE (menu_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "More actions", -1);

  /* Two flat rows in the popover, the way GNOME writes a menu of two
     things. */
  gtk_widget_add_css_class (toggle, "flat");
  gtk_widget_add_css_class (remove, "flat");
  gtk_widget_set_margin_top (items, 6);
  gtk_widget_set_margin_bottom (items, 6);
  gtk_widget_set_margin_start (items, 6);
  gtk_widget_set_margin_end (items, 6);
  gtk_box_append (GTK_BOX (items), toggle);
  gtk_box_append (GTK_BOX (items), remove);
  gtk_popover_set_child (GTK_POPOVER (popover), items);

  /* Each button carries the file it acts on. */
  g_object_set_data (G_OBJECT (toggle), "aml-file", file);
  g_object_set_data (G_OBJECT (remove), "aml-file", file);
  g_signal_connect (toggle, "clicked", G_CALLBACK (on_toggle_clicked), self);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_clicked), self);

  gtk_menu_button_set_popover (GTK_MENU_BUTTON (menu_button), popover);

  gtk_box_append (GTK_BOX (suffix), update);
  gtk_box_append (GTK_BOX (suffix), menu_button);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), suffix);

  /* The buttons on the row carry the file they act on, so nothing on
   * it has to know where the file came from. */
  g_object_set_data (G_OBJECT (row), "aml-file", file);
  g_object_set_data (G_OBJECT (row), "aml-update", update);
  g_object_set_data (G_OBJECT (update), "aml-file", file);
  g_signal_connect (update, "clicked", G_CALLBACK (on_update_clicked), self);

  file_row_refresh (row);

  return row;
}

/* Asks Modrinth what each file is, one after the other, so that a list
 * of twenty mods does not open twenty requests at once. */
typedef struct {
  AmlContentPage *page; /* weakly held */
  GCancellable *cancellable; /* the one the chain's requests are on */
  guint index;
  /* The file and the row the in-flight request is about. */
  AmlContentFile *file;
  GtkWidget *row;
} IdentifyRun;

static void identify_next (IdentifyRun *run);

static void
identify_run_free (IdentifyRun *run)
{
  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page),
                                  (gpointer *) &run->page);

  g_clear_object (&run->cancellable);
  g_free (run);
}

static void
identify_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  IdentifyRun *run = user_data;
  AmlContentPage *self = run->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthVersion) version = NULL;
  AmlContentFile *file = run->file;
  GtkWidget *row = run->row;

  version = aml_modrinth_version_by_hash_finish (result, &error);

  /* A new folder is being read now: the chain's cancellable is cancelled,
   * so the chain of the old answer stops. */
  if (run->cancellable != NULL &&
      g_cancellable_is_cancelled (run->cancellable))
    {
      identify_run_free (run);
      return;
    }

  if (self == NULL || file == NULL || row == NULL)
    {
      identify_run_free (run);
      return;
    }

  if (version != NULL)
    {
      file->project_id = g_strdup (version->project_id);
      file->version_name = g_strdup (version->name);
      file->version_number = g_strdup (version->version_number);

      if (file->project_id != NULL && *file->project_id != '\0')
        g_hash_table_add (self->projects, g_strdup (file->project_id));

      file_row_refresh (row);
    }

  /* Modrinth does not know this file, or the network is not there: it
   * keeps its own name and the page works all the same. */
  g_clear_error (&error);

  /* The answer for the file that was asked is in; the chain goes on. */
  identify_next (run);
}

static void
identify_next (IdentifyRun *run)
{
  AmlContentPage *self = run->page;

  if (self == NULL)
    {
      identify_run_free (run);
      return;
    }

  while (run->index < self->files->len)
    {
      AmlContentFile *file = g_ptr_array_index (self->files, run->index);
      g_autofree char *key = NULL;
      GtkWidget *row = NULL;

      run->index++;
      key = g_strdup (file->path);
      row = g_hash_table_lookup (self->rows, key);

      if (row == NULL || file->sha1 == NULL)
        continue;

      run->file = file;
      run->row = row;
      aml_modrinth_version_by_hash_async (file->sha1, run->cancellable,
                                          identify_ready, run);
      return;
    }

  identify_run_free (run);
}

static void
scan_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) files = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  files = aml_content_scan_finish (result, &error);

  if (files == NULL)
    {
      if (error != NULL && !g_error_matches (error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED))
        show_error (self, "The folder cannot be read", error);

      page_watch_free (watch);
      return;
    }

  /* The rows that pointed into the old list are gone, so the list they
   * pointed into can go too. */
  take_rows_out (&self->installed_rows,
                 ADW_PREFERENCES_GROUP (self->installed_group));
  g_clear_pointer (&self->files, g_ptr_array_unref);
  g_clear_pointer (&self->rows, g_hash_table_unref);
  self->files = g_steal_pointer (&files);
  self->rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < self->files->len; i++)
    {
      AmlContentFile *file = g_ptr_array_index (self->files, i);
      GtkWidget *row = file_row (self, file);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->installed_group),
                                 row);
      self->installed_rows = g_list_append (self->installed_rows, row);
      g_hash_table_insert (self->rows, g_strdup (file->path), row);
    }

  {
    gboolean empty = self->files->len == 0;

    gtk_widget_set_visible (self->installed_status, empty);
    gtk_widget_set_visible (self->installed_scroller, !empty);
    gtk_widget_set_sensitive (self->update_all, !empty);
  }

  {
    IdentifyRun *run = g_new0 (IdentifyRun, 1);

    run->page = self;
    g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
    run->cancellable = g_object_ref (self->scan);
    identify_next (run);
  }

  page_watch_free (watch);
}

static void
reload_installed (AmlContentPage *self)
{
  g_autofree char *directory = NULL;
  GCancellable *previous = self->scan;

  if (self->scan == NULL || self->installed_group == NULL)
    return;

  g_object_get (self->instance, "directory", &directory, NULL);

  if (directory == NULL)
    return;

  /* A scan that is still running is replaced: what it would answer is
   * about a folder that has since changed. */
  self->scan = g_cancellable_new ();
  g_cancellable_cancel (previous);
  g_clear_object (&previous);

  aml_content_scan_async (directory, self->type, self->scan, scan_ready,
                          page_watch_new (self));
}

static void
on_remove_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlContentFile *file = g_object_get_data (G_OBJECT (source), "aml-file");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
  g_autoptr (GFile) to_remove = NULL;
  g_autoptr (GError) error = NULL;

  if (file == NULL || g_strcmp0 (response, "remove") != 0)
    return;

  to_remove = g_file_new_for_path (file->path);

  if (!g_file_delete (to_remove, NULL, &error))
    {
      show_error (self, "The file cannot be removed", error);
      return;
    }

  show_toast (self, "Removed");
  reload_installed (self);
}

/* The newer version Modrinth has for one file, or the words that it has
 * nothing newer for. */
static void
on_update_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  AmlModrinthVersion *update = NULL;
  AmlContentFile *file = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  update = aml_modrinth_update_by_hash_finish (result, &error);
  file = g_object_get_data (G_OBJECT (self), "aml-update-file");

  set_busy (self, FALSE);

  if (update == NULL)
    {
      /* Nothing newer is the answer the page shows as up to date. */
      show_toast (self, error != NULL ? error->message : "Up to date");
      update_run_next (g_object_get_data (G_OBJECT (self), "aml-update-run"));
      page_watch_free (watch);
      return;
    }

  if (file == NULL)
    {
      aml_modrinth_version_free (update);
      page_watch_free (watch);
      return;
    }

  set_busy (self, TRUE);
  start_plan (self, update,
              file->version_name != NULL ? file->version_name : file->filename,
              file->path, !file->enabled);
  page_watch_free (watch);
}

/* The update of every file Modrinth knows, one after the other. */
static void
update_run_next (UpdateRun *run)
{
  AmlContentPage *self = run != NULL ? run->page : NULL;

  if (self == NULL)
    {
      g_free (run);
      return;
    }

  while (run->index < self->files->len)
    {
      AmlContentFile *file = g_ptr_array_index (self->files, run->index);

      run->index++;

      if (file->sha1 == NULL || file->project_id == NULL)
        continue;

      g_object_set_data (G_OBJECT (self), "aml-update-file", file);
      aml_modrinth_update_by_hash_async (self->type, file->sha1,
                                         self->minecraft_version,
                                         self->loader, self->browse,
                                         on_update_ready,
                                         page_watch_new (self));
      return;
    }

  g_object_set_data (G_OBJECT (self), "aml-update-run", NULL);
  g_free (run);
  set_busy (self, FALSE);
}

static void
on_update_all_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  UpdateRun *run = g_new0 (UpdateRun, 1);

  run->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
  g_object_set_data (G_OBJECT (self), "aml-update-run", run);
  set_busy (self, TRUE);
  update_run_next (run);
}

static void
on_update_clicked (GtkButton *button, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlContentFile *file = g_object_get_data (G_OBJECT (button), "aml-file");

  if (file == NULL || file->sha1 == NULL || file->project_id == NULL)
    return;

  g_object_set_data (G_OBJECT (self), "aml-update-file", file);
  set_busy (self, TRUE);
  aml_modrinth_update_by_hash_async (self->type, file->sha1,
                                     self->minecraft_version, self->loader,
                                     self->browse, on_update_ready,
                                     page_watch_new (self));
}

static void
open_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_default_for_uri_finish (result, &error))
    g_warning ("the folder does not open: %s", error->message);

  g_free (user_data);
}

static void
on_open_folder_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  g_autofree char *uri = NULL;

  /* The folder may not exist yet on a fresh instance: asking the desktop
     to open a path that is not there only confuses it. Create it first,
     empty, ready for files. */
  if (g_mkdir_with_parents (self->folder, 0755) != 0)
    {
      g_autofree char *message =
        g_strdup_printf ("Cannot create \"%s\": %s", self->folder,
                         g_strerror (errno));

      show_toast (self, message);
      return;
    }

  uri = g_filename_to_uri (self->folder, NULL, NULL);

  if (uri == NULL)
    return;

  /* GIO hands the folder to the desktop the way a double click in a file
   * manager does: nothing goes through a shell. */
  g_app_info_launch_default_for_uri_async (uri, NULL, NULL, open_finished,
                                           g_strdup (uri));
}

static void
on_refresh_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  reload_installed (user_data);
}

/* ---------------------------------------------------------- *
 * Browse
 * ---------------------------------------------------------- */

static void on_install_clicked (GtkButton *button, gpointer user_data);
static void on_result_activated (AdwActionRow *row, gpointer user_data);
static void on_category_toggled (GtkCheckButton *button, gpointer user_data);
static void on_scroll_changed (GtkAdjustment *adjustment, gpointer user_data);

/* The list is scrolled to its end: ask for the next page. Only one page
 * is ever on its way, and there is nothing to ask for once the total is
 * reached, so the signal is answered with a flag rather than by
 * counting.
 *
 * GTK4 has no edge-reached signal on GtkScrolledWindow, so the adjustment
 * is watched instead: a scroll to the bottom puts the value at the upper
 * bound. */
static void
on_scroll_changed (GtkAdjustment *adjustment, gpointer user_data)
{
  AmlContentPage *self = user_data;
  gdouble at_end;

  if (self->loading_page)
    return;

  if (self->offset >= self->total_hits)
    return;

  at_end = gtk_adjustment_get_upper (adjustment) -
           gtk_adjustment_get_page_size (adjustment) -
           gtk_adjustment_get_step_increment (adjustment);

  if (gtk_adjustment_get_value (adjustment) < at_end)
    return;

  self->offset += RESULTS_PER_PAGE;
  run_search (self, TRUE);
}

/* A page that failed can be asked for again from the row that says it
 * failed. The offset is already where the page starts, so the same
 * question is asked again. */
static void
on_retry_page_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  if (self->loading_page)
    return;

  gtk_widget_set_visible (self->retry_row, FALSE);
  run_search (self, TRUE);
}

/* The rows of a page of results. @append is FALSE for a new search or a
 * new filter, which starts the list over, and TRUE for the next page,
 * which is added to what is already there. */
/* Puts the project icon in @icon when it has been fetched, and leaves
   the placeholder in it when there is none. The widget is held for the
   length of the fetch, so a row that goes away first is still a widget
   when the answer arrives. */
static void
icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
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

static void
start_row_icon (GtkWidget *icon, const char *url)
{
  g_object_set_data_full (G_OBJECT (icon), "aml-icon-url", g_strdup (url),
                          g_free);
  aml_icon_load_async (url, NULL, icon_ready, g_object_ref (icon));
}

static void
results_arrived (AmlContentPage *self, AmlModrinthResults *results,
                 gboolean append)
{
  guint shown = 0;

  if (!append)
    {
      take_rows_out (&self->result_rows,
                     ADW_PREFERENCES_GROUP (self->results_group));
      g_clear_pointer (&self->results, g_ptr_array_unref);
      self->results = g_ptr_array_new_with_free_func (
          (GDestroyNotify) aml_modrinth_project_free);
      self->total_hits = results->total_hits;
      g_hash_table_remove_all (self->seen_ids);

      /* A new search starts at the top of the list. */
      {
        GtkAdjustment *adjustment =
          gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (
            self->results_scroller));

        gtk_adjustment_set_value (adjustment, gtk_adjustment_get_lower (adjustment));
      }
    }

  for (guint i = 0; i < results->projects->len; i++)
    {
      AmlModrinthProject *project = g_ptr_array_index (results->projects, i);
      GtkWidget *row = adw_action_row_new ();
      GtkWidget *icon = aml_icon_widget_new (
          aml_content_type_icon (self->type));
      GtkWidget *suffix = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
      GtkWidget *install = gtk_button_new_with_label ("Install");

      /* A project already listed is not listed twice: the same answer can
       * come back for two pages that overlap. */
      if (g_hash_table_contains (self->seen_ids, project->project_id))
        continue;

      g_hash_table_add (self->seen_ids, g_strdup (project->project_id));

      /* The icon, then the name and the description under it. Nothing
         else: who wrote it, how often it was taken, which game version
         and which loader are what the details page is for. Markup is
         turned off before the title and the description go in, because
         both are plain text from Modrinth and may hold an "&". */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                     project->title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), project->description);
      adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);

      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      start_row_icon (icon, project->icon_url);

      gtk_widget_add_css_class (install, "suggested-action");
      gtk_widget_set_valign (install, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (suffix), install);

      /* The rows point into the list the page owns, so the projects
       * live exactly as long as the rows that show them. */
      g_ptr_array_add (self->results, aml_modrinth_project_copy (project));
      g_object_set_data (G_OBJECT (row), "aml-project",
                         g_ptr_array_index (self->results, shown));
      g_object_set_data (G_OBJECT (install), "aml-project",
                         g_ptr_array_index (self->results, shown));

      adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), suffix);

      /* The whole row opens the project; the button installs the newest
       * version of it that fits this instance. An AdwActionRow is not
       * activatable by default, so the row itself is made one: a click
       * on it, anywhere but the install button, activates the row. */
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_signal_connect (row, "activated", G_CALLBACK (on_result_activated),
                        self);
      g_signal_connect (install, "clicked",
                        G_CALLBACK (on_install_clicked), self);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->results_group),
                                 row);
      self->result_rows = g_list_append (self->result_rows, row);
      shown++;
    }

  /* The offset is where the next page starts, which is the end of what
     is listed now. */
  self->offset += shown;

  /* The list does not grow without end: past the cap the oldest rows go,
     and the scroll is put back where it was, so the user stays on the
     same project rather than being thrown to the top. */
  if (self->result_rows != NULL &&
      g_list_length (self->result_rows) > MAX_RESULT_ROWS)
    {
      GtkAdjustment *adjustment =
        gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (
          self->results_scroller));
      gdouble at = gtk_adjustment_get_value (adjustment);
      gdouble removed = 0;

      for (guint i = 0; i < RESULT_ROWS_TRIM; i++)
        {
          GList *oldest = self->result_rows;
          GtkWidget *row = oldest->data;
          gint height = gtk_widget_get_height (row);

          removed += height > 0 ? height : 48;

          self->result_rows = g_list_delete_link (self->result_rows, oldest);
          adw_preferences_group_remove (ADW_PREFERENCES_GROUP (
                                          self->results_group),
                                        row);
        }

      gtk_adjustment_set_value (adjustment,
                                MAX (at - removed,
                                     gtk_adjustment_get_lower (adjustment)));
    }

  {
    gboolean empty = self->results->len == 0;

    gtk_widget_set_visible (self->results_status, empty);
    gtk_widget_set_visible (self->results_scroller, !empty);
    gtk_widget_set_visible (self->loading_more, FALSE);
    gtk_widget_set_visible (self->retry_row, FALSE);

    /* Nothing more to ask for once the total is reached. */
    if (self->offset >= self->total_hits)
      gtk_widget_set_visible (self->loading_more, FALSE);
  }
}

static void
search_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
              gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthResults) results = NULL;
  gboolean append = FALSE;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  append = self->pending_append;
  results = aml_modrinth_search_finish (result, &error);
  set_busy (self, FALSE);
  self->loading_page = FALSE;

  if (results == NULL)
    {
      take_rows_out (&self->result_rows,
                 ADW_PREFERENCES_GROUP (self->results_group));
      g_clear_pointer (&self->results, g_ptr_array_unref);
      gtk_widget_set_visible (self->results_status, TRUE);
      gtk_widget_set_visible (self->results_group, FALSE);
      gtk_widget_set_visible (self->loading_more, FALSE);

      /* A page that failed is offered again from the list itself. */
      if (append)
        {
          g_autofree char *message =
            error != NULL ? g_strdup (error->message)
                          : g_strdup ("the next page could not be loaded");

          gtk_label_set_text (GTK_LABEL (self->retry_label), message);
          gtk_widget_set_visible (self->retry_row, TRUE);
        }
      else if (error != NULL &&
               !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_autofree char *escaped = g_markup_escape_text (error->message, -1);

          adw_status_page_set_description (ADW_STATUS_PAGE (self->results_status),
                                           escaped);
        }

      page_watch_free (watch);
      return;
    }

  results_arrived (self, results, append);
  page_watch_free (watch);
}

static void
run_search (AmlContentPage *self, gboolean append)
{
  g_autoptr (AmlModrinthSearch) search = g_new0 (AmlModrinthSearch, 1);

  search->type = self->type;
  search->query = g_strdup (self->query != NULL ? self->query : "");
  search->minecraft_version = g_strdup (self->minecraft_version);
  search->loader = self->loader != NULL ? g_strdup (self->loader) : NULL;
  search->categories = self->categories != NULL
                           ? g_ptr_array_ref (self->categories)
                           : NULL;
  search->offset = self->offset;
  search->limit = RESULTS_PER_PAGE;

  /* A new search or a new filter cancels the page that was on its way
   * and starts the list over. */
  if (!append)
    {
      g_cancellable_cancel (self->more);
      g_clear_object (&self->more);
      self->more = g_cancellable_new ();
      self->offset = 0;
    }

  self->loading_page = TRUE;
  self->pending_append = append;
  set_busy (self, TRUE);

  /* The spinner at the bottom says a page is on its way. */
  gtk_widget_set_visible (self->loading_more, TRUE);
  gtk_widget_set_visible (self->retry_row, FALSE);

  aml_modrinth_search_async (search, self->more, search_ready,
                             page_watch_new (self));
}

static gboolean
on_search_timeout (gpointer user_data)
{
  AmlContentPage *self = user_data;

  self->debounce_source = 0;
  g_free (self->query);
  self->query =
    g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->search_entry)));
  self->offset = 0;
  run_search (self, FALSE);

  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  if (self->debounce_source != 0)
    g_source_remove (self->debounce_source);

  self->debounce_source =
    g_timeout_add (SEARCH_DELAY_MS, on_search_timeout, self);
}

/* Escape means "forget what I typed". The bar of this page is always
 * open, so there is nothing for Escape to close and it would be swallowed
 * without clearing anything.
 *
 * GTK4 has no key-press-event signal and no stop-searching signal on
 * GtkSearchEntry, so the key is taken with a controller of its own. It is
 * added after the one inside the search entry and both run on the bubble
 * phase, so this one is asked first.
 *
 * Clearing the text makes the entry emit "changed", which is what starts
 * the search again, this time for everything. */
static gboolean
on_search_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
               guint keyval, guint keycode G_GNUC_UNUSED,
               GdkModifierType state G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;

  if (*gtk_editable_get_text (
        GTK_EDITABLE (self->search_entry)) != '\0')
    gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");

  return GDK_EVENT_STOP;
}

static void categories_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void fill_categories (AmlContentPage *self, GPtrArray *categories);

static void
load_categories (AmlContentPage *self)
{
  g_autofree char *project_type =
    aml_modrinth_project_type_for_content (self->type);

  self->categories_loaded = TRUE;
  aml_modrinth_categories_async (project_type, self->browse,
                                 categories_ready, page_watch_new (self));
}

static void
load_categories_again (GtkWidget *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  self->categories_loaded = FALSE;
  load_categories (self);
}

/* Fills the popover with the categories, grouped under the heading
 * Modrinth puts them under, with readable labels. */
static void
fill_categories (AmlContentPage *self, GPtrArray *categories)
{
  GtkWidget *child = gtk_widget_get_first_child (self->filter_box);

  while (child != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      gtk_box_remove (GTK_BOX (self->filter_box), child);
      child = next;
    }

  {
    g_autofree char *current = NULL;

    for (guint i = 0; i < categories->len; i++)
      {
        AmlModrinthCategory *category = g_ptr_array_index (categories, i);
        GtkWidget *button = NULL;

        if (g_strcmp0 (current, category->header) != 0)
          {
            g_autofree char *heading_label =
              category->header != NULL
                  ? aml_modrinth_category_label (category->header)
                  : g_strdup ("Other");
            GtkWidget *heading = gtk_label_new (heading_label);

            g_free (current);
            current = g_strdup (category->header != NULL ? category->header
                                                        : "other");

            gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
            gtk_widget_add_css_class (heading, "caption");
            gtk_widget_add_css_class (heading, "dim-label");
            gtk_box_append (GTK_BOX (self->filter_box), heading);
          }

        {
          g_autofree char *label_text =
            aml_modrinth_category_label (category->name);

          button = gtk_check_button_new_with_label (label_text);
        }

        gtk_widget_add_css_class (button, "flat");
        g_object_set_data_full (G_OBJECT (button), "aml-category",
                                g_strdup (category->name), g_free);
        g_signal_connect (button, "toggled", G_CALLBACK (on_category_toggled),
                          self);
        gtk_box_append (GTK_BOX (self->filter_box), button);
      }
  }
}

static void
categories_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) categories = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  categories = aml_modrinth_categories_finish (result, &error);

  if (self->filter_box == NULL)
    {
      page_watch_free (watch);
      return;
    }

  /* A failed request is said so, with a way to try again: an empty list
   * of check buttons reads as "no categories", not as a failure. */
  if (categories == NULL)
    {
      GtkWidget *child = gtk_widget_get_first_child (self->filter_box);

      while (child != NULL)
        {
          GtkWidget *next = gtk_widget_get_next_sibling (child);

          gtk_box_remove (GTK_BOX (self->filter_box), child);
          child = next;
        }

      {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *label = gtk_label_new (
            error != NULL ? error->message : "Could not load categories");
        GtkWidget *retry = gtk_button_new_with_label ("Retry");

        gtk_label_set_wrap (GTK_LABEL (label), TRUE);
        gtk_widget_set_size_request (box, 220, -1);
        gtk_box_append (GTK_BOX (box), label);
        gtk_box_append (GTK_BOX (box), retry);

        gtk_box_append (GTK_BOX (self->filter_box), box);
        g_signal_connect (retry, "clicked",
                          G_CALLBACK (load_categories_again), self);
      }

      page_watch_free (watch);
      return;
    }

  fill_categories (self, categories);
  page_watch_free (watch);
}

static void
on_filter_clicked (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  if (self->categories_loaded)
    return;

  load_categories (self);
}

static void
on_category_toggled (GtkCheckButton *button, gpointer user_data)
{
  AmlContentPage *self = user_data;
  const char *category =
    g_object_get_data (G_OBJECT (button), "aml-category");

  if (category == NULL)
    return;

  if (gtk_check_button_get_active (button))
    g_ptr_array_add (self->categories, g_strdup (category));
  else
    {
      /* What is left of the filter, without the one that was turned
       * off. */
      GPtrArray *kept = g_ptr_array_new_with_free_func (g_free);

      for (guint i = 0; i < self->categories->len; i++)
        {
          const char *one = g_ptr_array_index (self->categories, i);

          if (g_strcmp0 (one, category) != 0)
            g_ptr_array_add (kept, g_strdup (one));
        }

      g_clear_pointer (&self->categories, g_ptr_array_unref);
      self->categories = kept;
    }

  self->offset = 0;
  run_search (self, FALSE);
}

static void
on_install_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlModrinthProject *project =
    g_object_get_data (G_OBJECT (button), "aml-project");

  if (project == NULL)
    return;

  g_object_set_data_full (G_OBJECT (self), "aml-pick-title",
                          g_strdup (project->title), g_free);
  set_busy (self, TRUE);
  aml_modrinth_versions_async (project->project_id, self->type,
                               self->minecraft_version, self->loader,
                               self->browse, on_pick_ready,
                               page_watch_new (self));
}

/* ---------------------------------------------------------- *
 * The page of one project
 * ---------------------------------------------------------- */

static void
on_version_install_clicked (GtkButton *button G_GNUC_UNUSED,
                            gpointer user_data G_GNUC_UNUSED)
{
  AmlContentPage *self = g_object_get_data (G_OBJECT (button), "aml-page");
  AmlModrinthVersion *version =
    g_object_get_data (G_OBJECT (button), "aml-version");

  if (self == NULL || version == NULL)
    return;

  set_busy (self, TRUE);
  start_plan (self, aml_modrinth_version_copy (version), version->name, NULL,
              FALSE);
}

static void
on_detail_versions_ready (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result, gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  AdwPreferencesGroup *group = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  group = self->details_versions;
  versions = aml_modrinth_versions_finish (result, &error);
  set_busy (self, FALSE);

  if (versions == NULL || group == NULL)
    {
      /* What a project publishes for nothing that fits is said in the
       * words the page shows everywhere else. */
      if (error != NULL && g_error_matches (error, AML_MODRINTH_ERROR,
                                            AML_MODRINTH_ERROR_NOT_FOUND))
        show_toast (self, error->message);

      page_watch_free (watch);
      return;
    }

  for (guint i = 0; i < versions->len; i++)
    {
      AmlModrinthVersion *version = g_ptr_array_index (versions, i);
      GtkWidget *row = adw_action_row_new ();
      GtkWidget *button = gtk_button_new_with_label ("Install");
      g_autofree char *subtitle = NULL;

      /* A name and a description are text, not markup: an "&" in one is
         an "&", so markup is turned off before anything is put in. */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                     version->name);
      /* The words a user reads under a version: what it is called, when
         it was published and how often it was taken. */
      subtitle = g_strdup_printf (
          "%s  ·  %s  ·  %" G_GUINT64_FORMAT " downloads",
          version->version_number[0] != '\0' ? version->version_number
                                             : version->version_id,
          version->date_published, version->downloads);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);

      gtk_widget_add_css_class (button, "suggested-action");
      gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
      g_object_set_data (G_OBJECT (button), "aml-version",
                         aml_modrinth_version_copy (version));
      g_object_set_data (G_OBJECT (button), "aml-page", self);
      g_signal_connect (button, "clicked",
                        G_CALLBACK (on_version_install_clicked), self);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
      adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), button);

      adw_preferences_group_add (group, row);
      self->detail_rows = g_list_append (self->detail_rows, row);
    }

  page_watch_free (watch);
}

static void
on_details_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthDetails) details = NULL;
  GtkWidget *body = NULL;
  GtkWidget *meta = NULL;
  g_autofree char *author = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  body = g_object_get_data (G_OBJECT (self), "aml-details-body");
  meta = g_object_get_data (G_OBJECT (self), "aml-details-meta");

  details = aml_modrinth_details_finish (result, &error);

  if (details == NULL)
    {
      set_busy (self, FALSE);

      /* The reason is shown where the body would have been, on the
         details page that is already open, and not only in a toast that
         may be behind it. */
      if (body != NULL)
        gtk_label_set_text (GTK_LABEL (body),
                            error != NULL ? error->message
                                          : "the project cannot be read");

      show_toast (self, error != NULL ? error->message
                                      : "the project cannot be read");
      page_watch_free (watch);
      return;
    }

  if (body != NULL)
    gtk_label_set_text (GTK_LABEL (body), details->body);

  author = g_strdup_printf ("%s  ·  by %s", details->license != NULL
                                                   ? details->license
                                                   : "",
                            details->author);
  if (meta != NULL)
    gtk_label_set_text (GTK_LABEL (meta), author);

  /* Only versions that fit this instance are asked for, and only those
   * can reach the list. */
  aml_modrinth_versions_async (self->details_project, self->type,
                               self->minecraft_version, self->loader,
                               self->browse, on_detail_versions_ready,
                               page_watch_new (self));

  page_watch_free (watch);
}

static void
on_result_activated (AdwActionRow *row G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlModrinthProject *project =
    g_object_get_data (G_OBJECT (row), "aml-project");
  GtkWidget *view = NULL;
  GtkWidget *scroller = NULL;
  GtkWidget *box = NULL;
  GtkWidget *heading = NULL;
  GtkWidget *title = NULL;
  GtkWidget *meta = NULL;
  GtkWidget *body = NULL;
  GtkWidget *page = NULL;
  g_autofree char *author = NULL;
  g_autofree char *downloads = NULL;

  if (project == NULL)
    return;

  /* The page of a project is pushed onto the navigation view of the
   * window, which is where every other page of the instance is. */
  view = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                  ADW_TYPE_NAVIGATION_VIEW);

  if (view == NULL)
    return;

  scroller = gtk_scrolled_window_new ();
  page = GTK_WIDGET (adw_navigation_page_new (scroller, project->title));
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  heading = adw_preferences_group_new ();

  title = gtk_label_new (project->title);
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_add_css_class (title, "title-2");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), title);

  author = g_strdup_printf ("by %s", project->author);
  downloads = format_downloads (project->downloads);
  meta = gtk_label_new (author);
  gtk_label_set_xalign (GTK_LABEL (meta), 0);
  gtk_widget_add_css_class (meta, "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), meta);

  body = gtk_label_new (project->description);
  gtk_label_set_wrap (GTK_LABEL (body), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (body), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (body), 0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), body);

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading),
                             gtk_label_new (downloads));

  self->details_versions = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (self->details_versions,
                                   "Compatible versions");
  gtk_widget_set_visible (GTK_WIDGET (self->details_versions), FALSE);
  self->details_project = g_strdup (project->project_id);

  g_object_set_data (G_OBJECT (self), "aml-details-body", body);
  g_object_set_data (G_OBJECT (self), "aml-details-meta", meta);

  gtk_box_append (GTK_BOX (box), heading);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->details_versions));
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box);
  gtk_widget_set_vexpand (scroller, TRUE);
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (page), project->title);

  /* The page of the project holds the page it was opened from, so it
   * stays alive for as long as it is on the screen. */
  g_object_set_data_full (G_OBJECT (page), "aml-content-page",
                          g_object_ref (self), g_object_unref);

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (view),
                            ADW_NAVIGATION_PAGE (page));

  set_busy (self, TRUE);
  aml_modrinth_details_async (project->project_id, self->browse,
                              on_details_ready, page_watch_new (self));
}

/* ---------------------------------------------------------- *
 * Building the page
 * ---------------------------------------------------------- */

static void
build_installed (AmlContentPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  AdwPreferencesGroup *group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  self->update_all = toolbar_button ("software-update-available-symbolic", "Update All",
                                     G_CALLBACK (on_update_all_clicked), self);
  gtk_widget_set_sensitive (self->update_all, FALSE);
  gtk_box_append (GTK_BOX (actions), self->update_all);
  gtk_box_append (GTK_BOX (actions),
                  toolbar_button ("folder-open-symbolic", "Open Folder",
                                  G_CALLBACK (on_open_folder_clicked), self));
  gtk_box_append (GTK_BOX (actions),
                  toolbar_button ("view-refresh-symbolic", "Refresh",
                                  G_CALLBACK (on_refresh_clicked), self));

  self->spinner = adw_spinner_new ();
  gtk_widget_set_valign (self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_box_append (GTK_BOX (actions), self->spinner);

  /* The actions belong to the toolbar row above both lists, not to this
     one of them: they are the same row whichever list is shown, and
     moving them keeps a download in progress from being thrown away by a
     visit to the other list.

     The row of the other list is taken out of the bar when the lists are
     switched, so this one is held here: without it, taking it out would
     be the last reference to it and would destroy the box and the
     spinner inside it while self->spinner still points at them. */
  self->installed_actions = actions;
  g_object_ref_sink (actions);

  self->installed_status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->installed_status),
                                 aml_content_type_icon (self->type));
  adw_status_page_set_title (ADW_STATUS_PAGE (self->installed_status),
                             "Nothing installed yet");
  adw_status_page_set_description (
      ADW_STATUS_PAGE (self->installed_status),
      "Browse for something to put into the folder of this instance.");

  self->installed_group = GTK_WIDGET (group);
  self->installed_scroller = scroller;
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 GTK_WIDGET (group));
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (content), self->installed_status);
  gtk_box_append (GTK_BOX (content), scroller);
  gtk_widget_set_visible (scroller, FALSE);
  gtk_box_append (GTK_BOX (box), content);

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack), box,
                                      "installed", "Installed",
                                      "view-list-symbolic");
}

/* The list of what Modrinth has for this instance is asked for when
 * Browse is looked at, not when the instance page is opened. */
/* Defined with the row it moves into, further down. */
static void move_actions_into_bar (AmlContentPage *self, gboolean browsing);

static void
on_stack_switched (GObject *stack, GParamSpec *pspec G_GNUC_UNUSED,
                   gpointer user_data)
{
  AmlContentPage *self = user_data;
  const char *visible = adw_view_stack_get_visible_child_name (
      ADW_VIEW_STACK (stack));
  gboolean browsing = g_strcmp0 (visible, "browse") == 0;

  /* The row shows the actions of whichever list is on top. */
  move_actions_into_bar (self, browsing);

  if (!browsing || self->browse_searched)
    return;

  self->browse_searched = TRUE;
  run_search (self, FALSE);
}

static void
build_browse (AmlContentPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *search_bar = gtk_search_bar_new ();
  GtkWidget *search_entry = gtk_search_entry_new ();
  GtkWidget *filter_button = gtk_menu_button_new ();
  GtkWidget *popover = gtk_popover_new ();
  GtkWidget *filter_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *filter_scroller = gtk_scrolled_window_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *loading_more = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *loading_spinner = gtk_spinner_new ();
  GtkWidget *loading_label = gtk_label_new ("Loading more…");
  GtkWidget *retry_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *retry_label = gtk_label_new (NULL);
  GtkWidget *retry_button = gtk_button_new_with_label ("Retry");
  AdwPreferencesGroup *group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  self->search_entry = search_entry;
  self->search_bar = search_bar;
  gtk_widget_set_hexpand (search_entry, TRUE);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_entry),
                                         "Search Modrinth");
  gtk_search_bar_set_child (GTK_SEARCH_BAR (search_bar), search_entry);
  gtk_search_bar_set_key_capture_widget (GTK_SEARCH_BAR (search_bar),
                                         GTK_WIDGET (self));

  /* Search is what this page is for, so the entry is there from the
     start: the user should be able to type without first asking for the
     box to appear. Ctrl+F and Escape still work on it. */
  gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (search_bar), TRUE);

  g_signal_connect (search_entry, "changed",
                    G_CALLBACK (on_search_changed), self);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_BUBBLE);
    gtk_widget_add_controller (search_entry, keys);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_search_key), self);
  }

  gtk_box_append (GTK_BOX (top), search_bar);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (filter_button),
                                 "view-continuous-symbolic");
  gtk_widget_set_tooltip_text (filter_button, "Filter by category");
  gtk_accessible_update_property (GTK_ACCESSIBLE (filter_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Filter",
                                  -1);
  gtk_widget_set_valign (filter_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (filter_button, "flat");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (filter_button), popover);
  /* A click opens the popover but does not emit "activate" on the
     menu button, so the load hangs off the popover's "show" instead. */
  g_signal_connect (popover, "show",
                    G_CALLBACK (on_filter_clicked), self);
  gtk_box_append (GTK_BOX (top), filter_button);

  self->filter_box = filter_box;
  gtk_widget_set_margin_top (filter_box, 6);
  gtk_widget_set_margin_bottom (filter_box, 6);
  gtk_widget_set_margin_start (filter_box, 6);
  gtk_widget_set_margin_end (filter_box, 6);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (filter_scroller),
                                 filter_box);
  gtk_widget_set_size_request (filter_scroller, 220, -1);
  gtk_popover_set_child (GTK_POPOVER (popover), filter_scroller);

  self->browse_actions = top;
  g_object_ref_sink (top);

  self->results_status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->results_status),
                                 "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                             "Nothing to show yet");
  adw_status_page_set_description (
      ADW_STATUS_PAGE (self->results_status),
      "Type what to look for, or browse everything Modrinth has for this "
      "instance.");
  self->results_group = GTK_WIDGET (group);
  self->results_scroller = scroller;
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 GTK_WIDGET (group));
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (content), self->results_status);
  gtk_box_append (GTK_BOX (content), scroller);
  gtk_widget_set_visible (scroller, FALSE);
  gtk_box_append (GTK_BOX (box), content);

  /* The bottom of the list: a spinner while the next page is on its way
     and a retry row when one failed. Neither is a button the user has to
     press to see more: the list asks for the next page itself when it is
     scrolled to the end. */
  gtk_widget_set_halign (loading_more, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (loading_more, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (loading_more, 12);
  gtk_widget_set_margin_bottom (loading_more, 12);
  gtk_widget_set_visible (loading_more, FALSE);
  gtk_box_append (GTK_BOX (loading_more), loading_spinner);
  gtk_box_append (GTK_BOX (loading_more), loading_label);
  adw_preferences_group_add (group, loading_more);

  gtk_widget_set_halign (retry_row, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (retry_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (retry_row, 12);
  gtk_widget_set_margin_bottom (retry_row, 12);
  gtk_widget_set_visible (retry_row, FALSE);
  gtk_label_set_wrap (GTK_LABEL (retry_label), TRUE);
  gtk_widget_add_css_class (retry_label, "dim-label");
  gtk_box_append (GTK_BOX (retry_row), retry_label);
  gtk_box_append (GTK_BOX (retry_row), retry_button);
  adw_preferences_group_add (group, retry_row);

  self->loading_more = loading_more;
  self->retry_row = retry_row;
  self->retry_label = retry_label;

  g_signal_connect (retry_button, "clicked",
                    G_CALLBACK (on_retry_page_clicked), self);

  /* The list asks for the next page when it is scrolled to the end.
     GTK4 has no edge-reached signal on GtkScrolledWindow, so the
     adjustment is watched instead: when the user scrolls to the bottom,
     the value is at the upper bound. */
  {
    GtkAdjustment *adjustment =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));

    g_signal_connect (adjustment, "value-changed",
                      G_CALLBACK (on_scroll_changed), self);
  }

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (self->stack), box,
                                      "browse", "Browse",
                                      "system-search-symbolic");
}

/* ---------------------------------------------------------- *
 * The row that decides between Installed and Browse
 *
 * A row of the page, not a second header bar. A header bar here would
 * carry a title and a close button over content that has neither, and
 * would put two bars between the top of the tab and the list. It is a
 * plain box: the actions of the list being shown on the left, the
 * switcher on the right, because which list is shown is what the right
 * end of a toolbar row is for.
 * ---------------------------------------------------------- */

static void
build_bar (AmlContentPage *self)
{
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  self->bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->switcher = adw_view_switcher_new ();

  /* The actions go in before this and the switcher after it, so the
     switcher ends up at the right end of the row however many actions
     there are. Aligning inside a box would not move it: a box gives every
     child its own width. */
  self->bar_spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (self->bar_spacer, TRUE);

  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (self->switcher),
                                ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (self->switcher),
                               ADW_VIEW_STACK (self->stack));
  gtk_widget_set_valign (self->switcher, GTK_ALIGN_CENTER);

  gtk_box_append (GTK_BOX (self->bar), self->bar_spacer);
  gtk_box_append (GTK_BOX (self->bar), self->switcher);

  gtk_widget_set_margin_top (self->bar, 6);
  gtk_widget_set_margin_bottom (self->bar, 6);
  gtk_widget_set_margin_start (self->bar, 12);
  gtk_widget_set_margin_end (self->bar, 12);

  gtk_box_append (GTK_BOX (inner), self->bar);
  gtk_box_append (GTK_BOX (inner), self->stack);

  /* One overlay over the row and the list, so a message about either one
     is said in the same place. */
  self->toasts = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toasts), inner);
  gtk_box_append (GTK_BOX (self), self->toasts);
}

/* The actions of the list being shown go into the row, at its left, and
   the ones of the other list come out. A widget with no parent is the one
   that belongs here, because only one list holds them at a time. */
static void
move_actions_into_bar (AmlContentPage *self, gboolean browsing)
{
  GtkWidget *wanted = browsing ? self->browse_actions
                               : self->installed_actions;
  GtkWidget *other = browsing ? self->installed_actions
                              : self->browse_actions;

  if (self->bar == NULL)
    return;

  if (other != NULL && gtk_widget_get_parent (other) == self->bar)
    gtk_box_remove (GTK_BOX (self->bar), other);

  /* At the front, which is the left end of the row: everything after it
     is the spacer and then the switcher. */
  if (wanted != NULL && gtk_widget_get_parent (wanted) != self->bar)
    gtk_box_insert_child_after (GTK_BOX (self->bar), wanted, NULL);
}

AmlContentPage *
aml_content_page_new (AmlInstance *instance, AmlContentType type)
{
  AmlContentPage *self = NULL;
  g_autofree char *directory = NULL;
  const char *loader = NULL;
  AmlLoader value = AML_LOADER_VANILLA;

  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);

  self = g_object_new (AML_TYPE_CONTENT_PAGE, NULL);
  self->instance = g_object_ref (instance);
  self->type = type;
  self->minecraft_version = instance_string (instance, "minecraft-version");
  self->browse = g_cancellable_new ();
  self->scan = g_cancellable_new ();
  self->transfer = g_cancellable_new ();
  self->more = g_cancellable_new ();
  self->seen_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          NULL);
  self->categories = g_ptr_array_new_with_free_func (g_free);
  self->projects = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          NULL);

  g_object_get (instance, "directory", &directory, NULL);

  if (directory != NULL)
    self->folder = aml_content_folder_path (directory, type);

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* Only a mod is decided by a loader: an instance that runs the game
   * itself has no mods to offer, and says so. */
  g_object_get (instance, "loader", &value, NULL);

  if (aml_content_type_needs_loader (type))
    {
      if (value == AML_LOADER_VANILLA)
        {
          GtkWidget *status = adw_status_page_new ();

          adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                         aml_content_type_icon (type));
          adw_status_page_set_title (ADW_STATUS_PAGE (status),
                                     "No mod loader");
          adw_status_page_set_description (
              ADW_STATUS_PAGE (status),
              "Mods need a mod loader. Pick Fabric, Quilt, Forge or NeoForge "
              "in the settings of this instance first.");
          gtk_box_append (GTK_BOX (self), status);

          return self;
        }
    }

  /* Modrinth reads its categories and loaders in lower case, which is
   * what aml_loader_to_string() writes. */
  g_object_get (instance, "loader", &value, NULL);
  loader = aml_loader_to_string (value);
  self->loader = loader != NULL ? g_strdup (loader) : NULL;

  self->stack = adw_view_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);

  build_installed (self);
  build_browse (self);

  g_signal_connect (self->stack, "notify::visible-child-name",
                    G_CALLBACK (on_stack_switched), self);

  build_bar (self);
  move_actions_into_bar (self, FALSE);

  reload_installed (self);

  return self;
}

static void
aml_content_page_dispose (GObject *object)
{
  AmlContentPage *self = AML_CONTENT_PAGE (object);

  /* The widget tree may be freed while the page is still answering a
   * request of its own: nothing of it is touched afterwards. */
  self->toasts = NULL;
  self->stack = NULL;
  self->bar = NULL;
  self->bar_spacer = NULL;
  self->switcher = NULL;
  /* The two action rows are held with a reference of their own, see
     build_installed() and build_browse(); giving it back is what lets
     them go when the page does. */
  g_clear_object (&self->installed_actions);
  g_clear_object (&self->browse_actions);
  self->installed_group = NULL;
  self->installed_scroller = NULL;
  self->installed_status = NULL;
  self->update_all = NULL;
  self->results_group = NULL;
  self->results_scroller = NULL;
  self->results_status = NULL;
  self->search_entry = NULL;
  self->search_bar = NULL;
  self->filter_box = NULL;
  self->loading_more = NULL;
  self->retry_row = NULL;
  self->retry_label = NULL;
  self->spinner = NULL;
  self->details_versions = NULL;

  if (self->debounce_source != 0)
    {
      g_source_remove (self->debounce_source);
      self->debounce_source = 0;
    }

  g_cancellable_cancel (self->browse);
  g_cancellable_cancel (self->scan);
  g_cancellable_cancel (self->transfer);
  g_cancellable_cancel (self->more);
  g_clear_object (&self->browse);
  g_clear_object (&self->scan);
  g_clear_object (&self->transfer);
  g_clear_object (&self->more);
  g_clear_pointer (&self->seen_ids, g_hash_table_unref);

  G_OBJECT_CLASS (aml_content_page_parent_class)->dispose (object);
}

static void
aml_content_page_finalize (GObject *object)
{
  AmlContentPage *self = AML_CONTENT_PAGE (object);

  g_clear_object (&self->instance);
  g_clear_pointer (&self->files, g_ptr_array_unref);
  g_clear_pointer (&self->rows, g_hash_table_unref);
  g_clear_pointer (&self->projects, g_hash_table_unref);
  g_clear_pointer (&self->categories, g_ptr_array_unref);
  g_clear_pointer (&self->results, g_ptr_array_unref);
  g_list_free (self->installed_rows);
  g_list_free (self->result_rows);
  g_list_free (self->detail_rows);
  g_free (self->minecraft_version);
  g_free (self->loader);
  g_free (self->folder);
  g_free (self->query);
  g_free (self->details_project);

  G_OBJECT_CLASS (aml_content_page_parent_class)->finalize (object);
}

static void
aml_content_page_class_init (AmlContentPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_content_page_dispose;
  object_class->finalize = aml_content_page_finalize;
}

static void
aml_content_page_init (AmlContentPage *self)
{
  /* Everything is built in aml_content_page_new(), which knows the
   * instance and the kind of content. */
  (void) self;
}