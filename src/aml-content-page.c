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
#include "aml-content-index.h"
#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-i18n.h"
#include "aml-icons.h"
#include "aml-markdown.h"

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
  GtkWidget *installed_actions;
  GtkWidget *installed_search_bar;
  GtkWidget *installed_search_entry;
  GtkWidget *browse_actions;

  GtkWidget *installed_group;
  GtkWidget *installed_scroller;
  GtkWidget *installed_status;
  GtkWidget *update_all;

  /* Update All first checks every file, then asks once. This is the list
     of updates it found and where applying them has got to. */
  gboolean updating;
  GPtrArray *updates; /* UpdateItem* */
  guint update_index;
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
  GHashTable *projects; /* project id -> NULL */
  GHashTable *project_icons; /* project id -> icon url the browser saw */
  GHashTable *project_descriptions; /* project id -> what it is */
  GHashTable *project_titles; /* project id -> project name */

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

  guint debounce_source;
  GCancellable *browse;   /* searching and the categories of the filter */
  GCancellable *scan;     /* the folder, its hashes and their lookups */
  gboolean identifying;   /* installed files are still being resolved */

  /* The page of a project waits for the description and the versions
     to arrive before showing anything. These are on the page so both
     callbacks can read them, unlike a temporary watch that gets freed. */
  gboolean details_ready;
  gboolean versions_ready;
  AmlModrinthDetails *details;         /* owned */
  GPtrArray *details_versions;         /* owned */
  char *details_problem;               /* why the description is not there */
  char *versions_problem;              /* why the versions are not there */

  /* What the page of the currently-open project is titled with and
     what it shows, so the fill function can rebuild the heading. */
  char *current_project_title;
  char *current_project_id; /* what the open project is, to toggle it shut */
  guint64 current_project_downloads;
  char *details_author;
  GtkWidget *details_body;
  GtkWidget *details_dropdown;
  GtkWidget *details_download;
  GtkWidget *details_page; /* weak: the page of the open project */
  GtkWidget *split;        /* the list on the left, the project on the right */

  /* The checkmark at the left of the versions of the dropdown: whether
     the version the dropdown shows is on the list. */
  GtkWidget *details_check;

  /* What the user picked: the versions whose checkmark is on, one entry
     per project. The button at the foot of the list installs the whole
     list at once. */
  GPtrArray *picks;  /* Pick* */
  GPtrArray *batch;  /* Pick*, the list being installed */
  gboolean picking;  /* the batch is being installed now */
  guint pick_index;
  GtkWidget *pick_button;
  GtkWidget *pick_badge;
};

G_DEFINE_FINAL_TYPE (AmlContentPage, aml_content_page, GTK_TYPE_BOX)

/* One file that has a newer version, and what to install for it. */
typedef struct {
  AmlModrinthVersion *version; /* the newer version, owned */
  char *title;
  char *path;
  gboolean disabled;
} UpdateItem;

/* The check for newer versions of everything Modrinth knows, and the
 * answer it built while it went. */
typedef struct {
  AmlContentPage *page; /* weakly held */
  guint index;
  GPtrArray *items; /* UpdateItem* */
} UpdateRun;

static void run_search (AmlContentPage *self, gboolean append);
static void reload_installed (AmlContentPage *self);
static void open_project_page (AmlContentPage *self, AmlModrinthProject *project);
static void update_check_next (UpdateRun *run);
static void update_batch_next (AmlContentPage *self);
static void start_plan (AmlContentPage *self, AmlModrinthVersion *version,
                        const char *title, const char *replaces,
                        gboolean disabled);
static const char *version_name_of (AmlModrinthVersion *version);
static AmlModrinthVersion *selected_version (GtkWidget *dropdown,
                                             GPtrArray *versions);
static void batch_next (AmlContentPage *self);
static void batch_stopped (AmlContentPage *self);
/* A plan that could not be carried out stops where it is; a batch it was
 * part of goes on to the next one. */
static void plan_stopped (AmlContentPage *self);
static void installed_icon_lookup (AmlContentPage *self, GtkWidget *row,
                                   const char *project_id);

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

/* Points @field at @widget and watches it: when the widget is destroyed
 * the field becomes NULL before anything can touch it. Used for the
 * widgets of the page of a project, which the page owns. */
static void
watch_widget (GtkWidget **field, GtkWidget *widget)
{
  if (*field != NULL)
    g_object_remove_weak_pointer (G_OBJECT (*field), (gpointer *) field);

  *field = widget;

  if (widget != NULL)
    g_object_add_weak_pointer (G_OBJECT (widget), (gpointer *) field);
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

/* Words that are not Markdown and must read exactly as they are: an
 * error, or a description with no markup of its own. */
static GtkWidget *
plain_label (const char *text)
{
  GtkWidget *label = gtk_label_new (text != NULL ? text : "");

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);

  return label;
}

static void
on_banner_dismiss (AdwBanner *banner G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  if (self->banner != NULL)
    adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
}

/* A failure is said in a banner that stays until it is read: a toast is
 * gone in seconds, and a file that could not be installed is worth more
 * than that. */
static void
show_error (AmlContentPage *self, const char *what, GError *error)
{
  g_autofree char *message = g_strdup_printf (
      "%s: %s", what,
      error != NULL ? error->message : "something went wrong");

  if (self->banner == NULL)
    return;

  adw_banner_set_use_markup (ADW_BANNER (self->banner), FALSE);
  adw_banner_set_title (ADW_BANNER (self->banner), message);
  adw_banner_set_revealed (ADW_BANNER (self->banner), TRUE);
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
  g_free (file->sha512);
  g_free (file->download_url);
  g_free (file->project_id);
  g_free (file->project_name);
  g_free (file->icon_url);
  g_free (file->version_id);
  g_free (file->version_name);
  g_free (file->version_number);
  g_clear_pointer (&file->required_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->optional_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->incompatible_dependencies, g_ptr_array_unref);
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
  char *instance_directory;
  AmlContentType type;
  char *extension;
  GPtrArray *files; /* AmlContentFile* */
  GPtrArray *index_entries; /* AmlContentIndexEntry* */
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
  g_clear_pointer (&run->index_entries, g_ptr_array_unref);
  g_free (run->instance_directory);
  g_free (run->extension);
  g_free (run);
}

static AmlContentIndexEntry *
scan_index_entry (ScanRun *run, AmlContentFile *file)
{
  g_autofree char *name = g_path_get_basename (file->path);

  for (guint i = 0; run->index_entries != NULL &&
                     i < run->index_entries->len; i++)
    {
      AmlContentIndexEntry *entry = g_ptr_array_index (run->index_entries, i);
      if (g_strcmp0 (entry->filename, name) == 0 &&
          g_strcmp0 (entry->hash, file->sha1) == 0)
        return entry;
    }
  return NULL;
}

static void
apply_index_entry (ScanRun *run, AmlContentFile *file)
{
  AmlContentIndexEntry *entry = scan_index_entry (run, file);

  if (entry == NULL)
    return;
  file->project_id = g_strdup (entry->project_id);
  file->sha512 = g_strdup (entry->hash512);
  file->download_url = g_strdup (entry->download_url);
  file->project_name = g_strdup (entry->project_name);
  file->icon_url = g_strdup (entry->icon_url);
  file->version_id = g_strdup (entry->version_id);
  file->version_name = g_strdup (entry->version_name);
  file->required_dependencies = g_ptr_array_ref (entry->required_dependencies);
  file->optional_dependencies = g_ptr_array_ref (entry->optional_dependencies);
  file->incompatible_dependencies = g_ptr_array_ref (entry->incompatible_dependencies);
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
scan_index_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  ScanRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) entries =
    aml_content_index_load_finish (result, &error);

  /* A missing or damaged index is deliberately not fatal: the existing
   * hash lookup repairs what it can, and the folder remains usable offline. */
  if (entries == NULL)
    {
      g_clear_error (&error);
      entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify) aml_content_index_entry_free);
    }
  run->index_entries = g_steal_pointer (&entries);

  g_file_enumerate_children_async (
    run->folder,
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
    g_task_get_cancellable (run->task), scan_children_ready, run);
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
      apply_index_entry (run, file);
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
  run->instance_directory = g_strdup (instance_directory);
  run->type = type;
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

  aml_content_index_load_async (instance_directory, type, cancellable,
                                scan_index_ready, run);
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
  char *sha512;
  char *project_id;
  char *project_name;
  char *version_id;
  char *version_name;
  GPtrArray *required_dependencies;
  GPtrArray *optional_dependencies;
  GPtrArray *incompatible_dependencies;
  char *label;
  char *replaces;
  gboolean required; /* a dependency of what was asked for, not it */
} InstallFile;

static void
install_file_free (InstallFile *file)
{
  if (file == NULL)
    return;

  g_free (file->url);
  g_free (file->filename);
  g_free (file->sha1);
  g_free (file->sha512);
  g_free (file->project_id);
  g_free (file->project_name);
  g_free (file->version_id);
  g_free (file->version_name);
  g_clear_pointer (&file->required_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->optional_dependencies, g_ptr_array_unref);
  g_clear_pointer (&file->incompatible_dependencies, g_ptr_array_unref);
  g_free (file->label);
  g_free (file->replaces);
  g_free (file);
}

/* The file of @version, named as it has to land in the folder. */
static InstallFile *
install_file_of (AmlModrinthVersion *version, const char *replaces,
                 gboolean disabled, const char *project_name)
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
  install->sha512 = g_strdup (file->sha512);
  install->project_id = g_strdup (version->project_id);
  install->project_name = g_strdup (project_name != NULL
                                      ? project_name : version->project_id);
  install->version_id = g_strdup (version->version_id);
  install->version_name = g_strdup (version->name);
  install->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  install->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  install->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; version->dependencies != NULL &&
                   i < version->dependencies->len; i++)
    {
      AmlModrinthDependency *dependency =
        g_ptr_array_index (version->dependencies, i);
      GPtrArray *target = dependency->type == AML_DEPENDENCY_REQUIRED
                            ? install->required_dependencies
                            : dependency->type == AML_DEPENDENCY_OPTIONAL
                                ? install->optional_dependencies
                                : dependency->type == AML_DEPENDENCY_INCOMPATIBLE
                                    ? install->incompatible_dependencies : NULL;
      if (target != NULL && dependency->project_id != NULL)
        g_ptr_array_add (target, g_strdup (dependency->project_id));
    }
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

static AmlContentFile *
installed_project_file (AmlContentPage *self, const char *project_id)
{
  if (self == NULL || project_id == NULL || self->files == NULL)
    return NULL;

  for (guint i = 0; i < self->files->len; i++)
    {
      AmlContentFile *file = g_ptr_array_index (self->files, i);

      if (g_strcmp0 (file->project_id, project_id) == 0)
        return file;
    }

  return NULL;
}

static void
remove_duplicate_project_files (AmlContentPage *self, const char *project_id,
                                const char *keep_path)
{
  if (self == NULL || project_id == NULL)
    return;

  for (guint i = 0; i < self->files->len; i++)
    {
      AmlContentFile *installed = g_ptr_array_index (self->files, i);
      g_autoptr (GFile) old = NULL;
      g_autoptr (GError) error = NULL;

      if (g_strcmp0 (installed->project_id, project_id) != 0 ||
          g_strcmp0 (installed->path, keep_path) == 0)
        continue;

      old = g_file_new_for_path (installed->path);
      if (!g_file_delete (old, NULL, &error) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_warning ("the duplicate file \"%s\" could not be removed: %s",
                   installed->path, error != NULL ? error->message : "unknown error");
      else
        {
          g_autofree char *directory = NULL;
          g_autofree char *filename = g_path_get_basename (installed->path);
          g_object_get (self->instance, "directory", &directory, NULL);
          aml_content_index_remove_async (directory, self->type, filename,
                                           NULL, NULL, NULL);
        }
    }
}

/* A queue of files written one after the other, so that a big file never
 * sits in front of the one that needs it and a failure stops where it
 * happened. The page is only weakly held: it can go away while the last
 * file of it is still on its way, and the folder is kept here so the
 * download finishes without it. */
typedef struct {
  AmlContentPage *page;   /* weak: the page can go during the run */
  char *folder;           /* where the files go, kept for the whole run */
  GPtrArray *files;       /* InstallFile* */
  guint index;
  AmlDownload *download;  /* the manager's entry, held while the run lives */
} FetchRun;

static void fetch_next (FetchRun *run);

static void
fetch_free (FetchRun *run)
{
  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page), (gpointer *) &run->page);

  if (run->download != NULL)
    aml_download_finish (run->download);

  g_clear_object (&run->download);
  g_free (run->folder);
  g_clear_pointer (&run->files, g_ptr_array_unref);
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
      gboolean cancelled = g_error_matches (error, AML_DOWNLOAD_ERROR,
                                            AML_DOWNLOAD_ERROR_CANCELLED);

      if (run->page != NULL)
        {
          AmlContentPage *page = run->page;

          /* A batch stops at the first file that fails rather than going
             on to the next with half of it missing. */
          if (page->updating)
            {
              g_clear_pointer (&page->updates, g_ptr_array_unref);
              page->update_index = 0;
              page->updating = FALSE;
            }

          if (page->picking)
            batch_stopped (page);

          set_busy (page, FALSE);

          /* A cancel is not a failure worth a message. */
          if (!cancelled)
            show_error (page, "The file could not be installed", error);
        }

      g_clear_error (&error);
      fetch_free (run);
      return;
    }

  /* What the header row shows: the phase line stays, the count on the
     right moves with each file landed. */
  if (run->download != NULL)
    aml_download_set_progress (run->download, run->index, run->files->len);

  /* The new version is in place, so the file it replaces can go. */
  if (file != NULL && file->replaces != NULL)
    {
      g_autoptr (GFile) old = g_file_new_for_path (file->replaces);

      if (!g_file_delete (old, NULL, NULL))
        g_warning ("the file \"%s\" it replaces could not be removed",
                   file->replaces);
    }

  if (run->page != NULL && file != NULL)
    {
      g_autofree char *keep = g_build_filename (run->folder, file->filename,
                                                 NULL);

      remove_duplicate_project_files (run->page, file->project_id, keep);

      {
        g_autofree char *instance_directory = NULL;
        AmlContentIndexEntry entry = { 0 };
        g_object_get (run->page->instance, "directory", &instance_directory,
                      NULL);
        entry.filename = file->filename;
         entry.hash = file->sha1;
         entry.hash512 = file->sha512;
         entry.download_url = file->url;
         entry.env_client = "required";
         entry.env_server = run->page->type == AML_CONTENT_MOD
                              ? "required" : "unsupported";
        entry.provider = "Modrinth";
        entry.project_id = file->project_id;
        entry.project_name = file->project_name;
        entry.version_id = file->version_id;
        entry.version_name = file->version_name;
        entry.required_dependencies = file->required_dependencies;
        entry.optional_dependencies = file->optional_dependencies;
         entry.incompatible_dependencies = file->incompatible_dependencies;
         entry.env_client = "required";
         entry.env_server = run->page->type == AML_CONTENT_MOD
                              ? "required" : "unsupported";
        aml_content_index_update_async (
          instance_directory, run->page->type, &entry,
          aml_download_get_cancellable (run->download), NULL, NULL);
      }
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
          if (self->updating)
            /* One update of the batch is in: start the next one. The
               list is read once, when the whole batch is done. */
            update_batch_next (self);
          else if (self->picking)
            /* One pick of the batch is in: the next one follows. */
            batch_next (self);
          else
            {
              set_busy (self, FALSE);
              reload_installed (self);
            }
        }

      fetch_free (run);
      return;
    }

  file = g_ptr_array_index (run->files, run->index);
  folder = g_file_new_for_path (run->folder);

  {
    g_autoptr (GError) error = NULL;

    if (!g_file_make_directory_with_parents (folder, NULL, &error) &&
        !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
      {
        if (self != NULL)
          {
            set_busy (self, FALSE);
            show_error (self, "The folder cannot be created", error);
          }

        fetch_free (run);
        return;
      }
  }

  path = g_build_filename (run->folder, file->filename, NULL);
  run->index++;

  aml_download_file_async (file->url, path, file->sha1, G_CHECKSUM_SHA1,
                           aml_download_get_cancellable (run->download), NULL,
                           fetch_ready, run);
}

static AmlDownloadKind
content_download_kind (AmlContentType type)
{
  switch (type)
    {
    case AML_CONTENT_RESOURCE_PACK:
      return AML_DOWNLOAD_RESOURCE_PACK;

    case AML_CONTENT_SHADER_PACK:
      return AML_DOWNLOAD_SHADER;

    case AML_CONTENT_MOD:
    default:
      return AML_DOWNLOAD_MOD;
    }
}

/* Takes the files of an install and writes them in order. The run owns
 * @files from here. */
static void
start_fetch (AmlContentPage *self, GPtrArray *files)
{
  FetchRun *run = g_new0 (FetchRun, 1);
  guint count = files != NULL ? files->len : 0;

  run->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
  run->files = files;
  run->folder = g_strdup (self->folder);

  {
    const char *title = NULL;

    if (files != NULL && files->len > 0)
      title = ((InstallFile *) g_ptr_array_index (files, 0))->label;

    if (title == NULL)
      title = self->current_project_title;

    if (title == NULL)
      title = "Content";

    /* Keep our own reference: aml_download_finish() removes the manager's
       reference when the run ends, while fetch_free() still releases the
       reference held by the run below. */
    run->download = g_object_ref (
        aml_downloads_add (content_download_kind (self->type), title));
    aml_download_set_detail (run->download, "Installing");
    aml_download_set_progress (run->download, 0, count);
  }

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
                                           plan->stays_disabled, plan->title);

      if (own != NULL)
        g_ptr_array_add (plan->files, own);

      show_plan (self, plan);
      return;
    }

  /* The dependency is taken out of the queue without being freed: the
     call below reads its project id, and freeing it first would be a
     read of memory that is gone. It is freed once the call has copied
     what it needs. */
  dependency = g_ptr_array_steal_index (plan->queue, 0);

  aml_modrinth_versions_async (dependency->project_id, self->type,
                               self->minecraft_version, self->loader,
                               plan->cancellable, plan_versions_ready, plan);
  aml_modrinth_dependency_free (dependency);
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

/* What the user is asked to confirm: the mods the install also needs,
 * one per line, and any conflict said in as many words. NULL when there
 * is nothing beside the file itself to say, which is the common case. */
static char *
plan_text (AmlContentPage *self, PlanRun *plan)
{
  g_autoptr (GString) text = g_string_new (NULL);
  gboolean listed = FALSE;

  for (guint i = 0; i < plan->files->len; i++)
    {
      InstallFile *file = g_ptr_array_index (plan->files, i);

      if (!file->required)
        continue;

      if (!listed)
        {
           g_string_append_printf (text, "%s\n", _("Dependencies:"));
          listed = TRUE;
        }

       {
         const char *title = file->project_id != NULL
                               ? g_hash_table_lookup (self->project_titles,
                                                      file->project_id)
                               : NULL;

         g_string_append_printf (text, "%s\n",
                                 title != NULL ? title : file->label);
       }
    }

  if (plan->version->dependencies != NULL)
    {
      g_autoptr (GString) notes = g_string_new (NULL);

      for (guint i = 0; i < plan->version->dependencies->len; i++)
        {
          AmlModrinthDependency *dependency =
            g_ptr_array_index (plan->version->dependencies, i);

          if (dependency->type != AML_DEPENDENCY_INCOMPATIBLE)
            continue;

          g_string_append_printf (
              notes,
              "It does not run with %s, which this instance already has.\n",
              dependency->project_id != NULL ? dependency->project_id
                                              : "another file");
        }

      if (notes->len > 0)
        {
          if (text->len > 0)
            g_string_append_c (text, '\n');

          g_string_append (text, "Conflicts\n");
          g_string_append (text, notes->str);
        }
    }

  while (text->len > 0 && text->str[text->len - 1] == '\n')
    g_string_truncate (text, text->len - 1);

  if (text->len == 0)
    return NULL;

  return g_string_free (g_steal_pointer (&text), FALSE);
}

static void
on_plan_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlContentPage *self = user_data;
  PlanRun *plan = g_object_get_data (G_OBJECT (source), "aml-plan");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (plan == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (g_strcmp0 (response, "install") != 0 || plan->page == NULL)
    {
      plan_free (plan);
      g_object_unref (self);
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
  g_object_unref (self);
}

static void
show_plan (AmlContentPage *self, PlanRun *plan)
{
  /* An update of the whole list was confirmed once, as a whole: this plan
     is not asked for again, it is simply carried out. So is one pick of a
     confirmed batch of them. */
  if (self->updating || self->picking)
    {
      start_fetch (self, plan->files);
      plan->files = NULL; /* the fetch owns them now */
      plan_free (plan);
      return;
    }

  {
    const char *action = plan->replaces != NULL ? _("Update") : _("Install");
    /* The name Modrinth writes already holds the version: "Xaero's
       Minimap 26.6.0". */
    const char *title = plan->title != NULL ? plan->title : "the file";
       g_autofree char *heading =
       g_strdup_printf (_("%s \"%s\""),
                        action == NULL ? "Install" : action, title);
     g_autofree char *body = plan_text (self, plan);
    AdwAlertDialog *alert =
      ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body));

    adw_alert_dialog_set_heading_use_markup (alert, FALSE);
    adw_alert_dialog_set_body_use_markup (alert, FALSE);
     adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
     adw_alert_dialog_add_response (alert, "install", _("Confirm"));
    adw_alert_dialog_set_response_appearance (alert, "install",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (alert, "cancel");
    adw_alert_dialog_set_close_response (alert, "cancel");

    g_object_set_data (G_OBJECT (alert), "aml-plan", plan);
    adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_plan_chosen,
                             g_object_ref (self));
  }
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
          plan_stopped (self);
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
          plan_stopped (self);
        }

      plan_free (plan);
      return;
    }

  /* The list is newest first, so the first is what an install takes. */
  version = g_ptr_array_steal_index (versions, 0);
  {
    AmlContentFile *installed = installed_project_file (self,
                                                         version->project_id);

    file = install_file_of (version, installed != NULL ? installed->path : NULL,
                            installed != NULL && !installed->enabled,
                            g_hash_table_lookup (self->project_titles,
                                                 version->project_id));
  }

  if (file != NULL)
    {
      /* This one is required by what was asked for, which is what the
         confirmation lists apart from the file itself. */
      file->required = TRUE;
      g_ptr_array_add (plan->files, file);
    }

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
  if (replaces == NULL)
    {
      AmlContentFile *installed = installed_project_file (self,
                                                           version->project_id);

      replaces = installed != NULL ? installed->path : NULL;
      disabled = installed != NULL && !installed->enabled;
    }
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

/* ---------------------------------------------------------- *
 * What the user picked
 *
 * A version whose checkmark is on joins a list, and the button at the
 * foot of the list installs the whole of it at once. A project is one
 * entry of that list: picking another version of a project that is
 * already on it replaces the one that was there, because installing the
 * same project twice would put two files in the folder.
 *
 * The list is not a download and does not stop anything: it waits as
 * long as the page lives, and the confirmation is asked once for the
 * whole of it, the way Update All asks once for every update it found.
 * ---------------------------------------------------------- */

/* One entry of the list: which version of which project, and the words
   the confirmation and the download call it by. */
typedef struct {
  char *project_id;
  char *title;
  AmlModrinthVersion *version; /* owned */
} Pick;

static void
pick_free (Pick *pick)
{
  if (pick == NULL)
    return;

  aml_modrinth_version_free (pick->version);
  g_free (pick->project_id);
  g_free (pick->title);
  g_free (pick);
}

/* The entry of @project_id in the list, or NULL when the project has
   nothing picked. */
static Pick *
pick_of_project (AmlContentPage *self, const char *project_id)
{
  if (self->picks == NULL || project_id == NULL)
    return NULL;

  for (guint i = 0; i < self->picks->len; i++)
    {
      Pick *pick = g_ptr_array_index (self->picks, i);

      if (g_strcmp0 (pick->project_id, project_id) == 0)
        return pick;
    }

  return NULL;
}

/* Whether @pick holds @version itself rather than another version of the
   same project: the copy in the list is not the one the page shows, so
   the ids are what say they are the same. */
static gboolean
pick_is_version (Pick *pick, AmlModrinthVersion *version)
{
  if (pick == NULL || version == NULL || pick->version == NULL)
    return FALSE;

  return g_strcmp0 (pick->version->version_id, version->version_id) == 0;
}

/* The checkmark at the left of the versions follows the list: the version
   the dropdown shows is on the list, and the button says so. Its state is
   set with the click handler held back, which is what keeps the two from
   calling each other. */
static void
refresh_pick_check (AmlContentPage *self)
{
  AmlModrinthVersion *version = NULL;
  gboolean on;

  if (self->details_check == NULL || self->details_dropdown == NULL)
    return;

  version = selected_version (self->details_dropdown,
                              self->details_versions);
  on = pick_is_version (pick_of_project (
                          self, version != NULL ? version->project_id : NULL),
                        version);

  g_object_set_data (G_OBJECT (self->details_check), "aml-setting",
                     GINT_TO_POINTER (1));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->details_check), on);
  g_object_set_data (G_OBJECT (self->details_check), "aml-setting", NULL);
}

/* What the page has picked is said to the header, which counts it on the
   icon of its downloads button, and put on the button at the foot of the
   list. Called after every change of the list, and once when the list is
   built, so that the button starts where the picks start. */
static void
refresh_picks (AmlContentPage *self)
{
  guint count = self->picks != NULL ? self->picks->len : 0;
  if (self->pick_badge != NULL)
    {
      g_autofree char *number = g_strdup_printf (
          "%" G_GUINT64_FORMAT, (guint64) count);

      gtk_label_set_text (GTK_LABEL (self->pick_badge),
                          count > 0 ? number : "");
      gtk_widget_set_visible (self->pick_badge, count > 0);
      gtk_accessible_update_property (GTK_ACCESSIBLE (self->pick_badge),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      number, -1);
    }

  if (self->pick_button != NULL)
    {
      g_autofree char *tooltip = NULL;

      if (count > 0)
        tooltip = g_strdup_printf (_("Install the selected %s?"),
                                   aml_content_type_title (self->type));

      gtk_widget_set_sensitive (self->pick_button, count > 0);
      gtk_widget_set_tooltip_text (
           self->pick_button,
           count == 0 ? _("Nothing is selected")
                      : tooltip);
    }

  refresh_pick_check (self);
}

/* @version of @project_id is now picked: it replaces whatever that
   project had on the list, or joins it. */
static void
pick_add (AmlContentPage *self, AmlModrinthVersion *version)
{
  Pick *pick = NULL;

  if (self->picks == NULL || version == NULL || version->project_id == NULL)
    return;

  pick = pick_of_project (self, version->project_id);

  if (pick != NULL)
    {
      /* The very version that was already on the list is left alone, so
         the checkmarks do not move under the click that put it there. */
      if (pick_is_version (pick, version))
        return;

      g_ptr_array_remove (self->picks, pick);
    }

  pick = g_new0 (Pick, 1);
  pick->project_id = g_strdup (version->project_id);
  pick->title = g_strdup (self->current_project_title != NULL
                            ? self->current_project_title
                            : version_name_of (version));
  pick->version = aml_modrinth_version_copy (version);
  g_ptr_array_add (self->picks, pick);
}

/* The project of @version has nothing picked any more. Only its own
   entry goes: another version of the same project is a different pick. */
static void
pick_remove (AmlContentPage *self, AmlModrinthVersion *version)
{
  Pick *pick = NULL;

  if (self->picks == NULL || version == NULL)
    return;

  pick = pick_of_project (self, version->project_id);

  if (pick_is_version (pick, version))
    g_ptr_array_remove (self->picks, pick);
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

  version = selected_version (self->details_dropdown, self->details_versions);

  if (version != NULL)
    {
      if (gtk_check_button_get_active (check))
        pick_add (self, version);
      else
        pick_remove (self, version);
    }

  /* What the list holds is what decides the state of the button, so a
     click that could not be carried out leaves it where it was. */
  refresh_picks (self);
}

/* Another version is what the dropdown shows: the checkmark at its left
   says whether that one is the one on the list. */
static void
on_version_selected (GObject *object G_GNUC_UNUSED,
                     GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  refresh_pick_check (self);
  refresh_picks (self);
}

/* What the confirmation is answered with. The list was taken out of the
   page when the question was asked, so what is installed is the list the
   user saw and nothing that is picked since. */
static void
on_batch_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlContentPage *self = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "install") != 0)
    {
      /* Nothing was installed, so the list the question was about goes
         back to the page with its checkmarks as they were. */
      if (self->batch != NULL)
        {
          g_clear_pointer (&self->picks, g_ptr_array_unref);
          self->picks = g_steal_pointer (&self->batch);
          refresh_picks (self);
        }

      g_object_unref (self);
      return;
    }

  if (self->batch == NULL)
    {
      g_object_unref (self);
      return;
    }

  self->picking = TRUE;
  self->pick_index = 0;
  batch_next (self);

  g_object_unref (self);
}

/* Starts the next version of the confirmed list, or says that the whole
   of it is in. Each one is planned and fetched like any other install;
   the confirmation is not asked again for the ones that follow. */
static void
batch_next (AmlContentPage *self)
{
  Pick *pick = NULL;

  if (self->batch == NULL || self->pick_index >= self->batch->len)
    {
      guint done = self->batch != NULL ? self->batch->len : 0;

      g_clear_pointer (&self->batch, g_ptr_array_unref);
      self->picking = FALSE;
      self->pick_index = 0;
      set_busy (self, FALSE);

      if (done > 0)
        {
          g_autofree char *message = g_strdup_printf (
              "Installed %u %s", done, done == 1 ? "item" : "items");

          show_toast (self, message);
          reload_installed (self);
        }

      return;
    }

  pick = g_ptr_array_index (self->batch, self->pick_index);
  self->pick_index++;

  set_busy (self, TRUE);
  start_plan (self, aml_modrinth_version_copy (pick->version), pick->title,
              NULL, FALSE);
}

/* A file of the batch could not be installed: what it would take is not
   there, so the list goes no further. */
static void
batch_stopped (AmlContentPage *self)
{
  g_clear_pointer (&self->batch, g_ptr_array_unref);
  self->picking = FALSE;
  self->pick_index = 0;
}

static void
clear_picks (AmlContentPage *self)
{
  g_clear_pointer (&self->picks, g_ptr_array_unref);
  self->picks = g_ptr_array_new_with_free_func ((GDestroyNotify) pick_free);
  refresh_picks (self);
}

/* The button at the foot of the list: every picked version, one per
   line, and the choice to install the whole of it at once. */
static void
on_pick_button_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  g_autoptr (GString) body = NULL;
  g_autoptr (GHashTable) listed = NULL;
  AdwAlertDialog *alert = NULL;
  const char *kind = NULL;

  if (self->picks == NULL || self->picks->len == 0)
    return;

  body = g_string_new (NULL);
  listed = g_hash_table_new (g_str_hash, g_str_equal);

  kind = aml_content_type_title (self->type);

  for (guint i = 0; i < self->picks->len; i++)
    {
      Pick *pick = g_ptr_array_index (self->picks, i);

      if (i > 0)
        g_string_append_c (body, '\n');

      g_string_append (body, pick->title);
      g_hash_table_add (listed, pick->project_id);
    }

  /* Show the required dependencies in their own section. These are the
     dependencies published for the selected versions, not optional or
     incompatible projects. The resolver still chooses the compatible
     version when the install starts. */
  {
    gboolean has_dependencies = FALSE;
    GString *dependencies = g_string_new (NULL);

    for (guint i = 0; i < self->picks->len; i++)
      {
        Pick *pick = g_ptr_array_index (self->picks, i);

        for (guint j = 0; pick->version->dependencies != NULL &&
                         j < pick->version->dependencies->len; j++)
          {
            AmlModrinthDependency *dependency =
              g_ptr_array_index (pick->version->dependencies, j);
            const char *title = NULL;

            if (dependency->type != AML_DEPENDENCY_REQUIRED ||
                dependency->project_id == NULL ||
                g_hash_table_contains (listed, dependency->project_id))
              continue;

            g_hash_table_add (listed, dependency->project_id);
            title = g_hash_table_lookup (self->project_titles,
                                         dependency->project_id);
            if (has_dependencies)
              g_string_append_c (dependencies, '\n');
            g_string_append_printf (dependencies, "%s",
                                    title != NULL ? title
                                                  : dependency->project_id);
            has_dependencies = TRUE;
          }
      }

    if (has_dependencies)
      {
        g_string_append_printf (body, "\n%s\n", _("Dependencies:"));
        g_string_append (body, dependencies->str);
      }

    g_string_free (dependencies, TRUE);
    }

  {
    g_autofree char *heading =
      g_strdup_printf (_("Install the selected %s?"), kind);

    alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body->str));
  }

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "install", _("Install"));
  adw_alert_dialog_set_response_appearance (alert, "install",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  /* The list goes into the batch here: what is installed is the list
     this question is about, whatever is picked while it is up. */
  g_clear_pointer (&self->batch, g_ptr_array_unref);
  self->batch = g_steal_pointer (&self->picks);
  self->picks = g_ptr_array_new_with_free_func ((GDestroyNotify) pick_free);
  refresh_picks (self);

  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_batch_chosen,
                           g_object_ref (self));
}

/* ---------------------------------------------------------- *
 * Installed: the rows and what can be done to them
 * ---------------------------------------------------------- */

/* Disables or enables one file by renaming it, and updates the one row it
 * is about. The folder is not read again: a rescan rebuilds every row and
 * asks Modrinth about every file a second time, and until it finished the
 * file still said it was enabled, so a fast second toggle tried to rename
 * a path that had already moved. TRUE when the file moved. */
static gboolean
toggle_file (AmlContentPage *self, AmlContentFile *file)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *renamed = NULL;
  g_autofree char *old_path = g_strdup (file->path);
  GtkWidget *row = NULL;

  renamed = aml_content_set_disabled (file->path, !file->enabled, &error);

  if (renamed == NULL)
    {
      show_error (self, "The file cannot be renamed", error);
      return FALSE;
    }

  g_free (file->path);
  file->path = g_steal_pointer (&renamed);
  file->enabled = !file->enabled;

  /* The row the file is about follows the new path, and the table that
     finds a row by its path is told, so nothing else has to move. */
  row = g_hash_table_lookup (self->rows, old_path);

  if (row != NULL)
    {
      g_hash_table_remove (self->rows, old_path);
      g_hash_table_insert (self->rows, g_strdup (file->path), row);
    }

  show_toast (self, file->enabled ? "Enabled" : "Disabled");

  return TRUE;
}

static void
installed_filter_rows (AmlContentPage *self)
{
  const char *query = self->installed_search_entry != NULL
                        ? gtk_editable_get_text (
                            GTK_EDITABLE (self->installed_search_entry))
                        : "";
  g_autofree char *needle = g_utf8_casefold (query, -1);

  for (GList *item = self->installed_rows; item != NULL; item = item->next)
    {
      GtkWidget *row = item->data;
      AmlContentFile *file = g_object_get_data (G_OBJECT (row), "aml-file");
      const char *title = NULL;
      g_autofree char *haystack = NULL;
      gboolean visible = TRUE;

      if (file != NULL && file->project_id != NULL)
        title = g_hash_table_lookup (self->project_titles, file->project_id);
      if (title == NULL && file != NULL)
        title = file->filename;

      if (needle != NULL && *needle != '\0')
        {
          g_autofree char *title_folded =
            g_utf8_casefold (title != NULL ? title : "", -1);
          g_autofree char *file_folded =
            g_utf8_casefold (file != NULL ? file->filename : "", -1);

          haystack = g_strconcat (title_folded != NULL ? title_folded : "",
                                  " ", file_folded != NULL ? file_folded : "",
                                  NULL);
          visible = strstr (haystack, needle) != NULL;
        }

      gtk_widget_set_visible (row, visible);
    }
}

static void
on_installed_search_changed (GtkEditable *editable G_GNUC_UNUSED,
                             gpointer user_data)
{
  installed_filter_rows (user_data);
}

static void
file_row_refresh (GtkWidget *row)
{
  AmlContentFile *file = g_object_get_data (G_OBJECT (row), "aml-file");
  AmlContentPage *self = g_object_get_data (G_OBJECT (row), "aml-page");
  const char *title = NULL;

  if (file == NULL || !GTK_IS_WIDGET (row))
    return;

  if (self != NULL && file->project_id != NULL)
    title = g_hash_table_lookup (self->project_titles, file->project_id);

  /* What Modrinth knows the project by, or the name on disk when it is
   * not known. The version remains useful as a subtitle/fallback only. */
  adw_preferences_row_set_title (
       ADW_PREFERENCES_ROW (row),
       title != NULL ? title : file->filename);
  installed_filter_rows (self);
}

/* The first @max_sentences sentences of @text (0 for all of it), with
 * runs of whitespace folded into single spaces. A row caps its subtitle
 * at two lines by wrapping, and an explicit blank line in the text would
 * slip a third one past that. A terminator only ends a sentence at the
 * end of the text or before a space, so "3.5" and "e.g." do not count. */
static char *
description_line (const char *text, guint max_sentences)
{
  g_autoptr (GString) out = g_string_new (NULL);
  guint sentences = 0;
  gboolean space = FALSE;
  gsize length = 0;

  if (text == NULL)
    return g_strdup ("");

  length = strlen (text);

  for (gsize i = 0; i < length; i++)
    {
      char c = text[i];

      if (g_ascii_isspace (c))
        {
          space = TRUE;
          continue;
        }

      if (space && out->len > 0)
        g_string_append_c (out, ' ');

      space = FALSE;
      g_string_append_c (out, c);

      if (max_sentences != 0 &&
          (c == '.' || c == '!' || c == '?') &&
          (i + 1 == length || g_ascii_isspace (text[i + 1])))
        {
          sentences++;

          if (sentences >= max_sentences)
            break;
        }
    }

  return g_string_free (g_steal_pointer (&out), FALSE);
}

/* What an installed file's project is, under its name. What the file is
 * called and where it lives is what the row's tooltip is for. */
static void
row_show_description (GtkWidget *row, const char *description)
{
  g_autofree char *short_text = NULL;

  if (row == NULL || description == NULL || *description == '\0')
    return;

  short_text = description_line (description, 2);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), short_text);
}

/* The switch of a mod row: on means the mod loads, off means its file
 * carries the disabled suffix. The rename updates the row it is about, so
 * only the switch the user touched moves. When the rename fails the
 * switch stays where it was, rather than showing a state the file does
 * not have. */
static gboolean
on_mod_toggled (GtkSwitch *sw, gboolean state G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlContentFile *file = g_object_get_data (G_OBJECT (sw), "aml-file");

  if (file == NULL || !toggle_file (self, file))
    return TRUE;

  gtk_widget_set_tooltip_text (GTK_WIDGET (sw),
                               file->enabled ? "Disable" : "Enable");

  return FALSE;
}

static void
on_deleted (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlContentPage *self = user_data;
  const char *path = g_object_get_data (G_OBJECT (source), "aml-path");
  g_autoptr (GError) error = NULL;

  if (!g_file_delete_finish (G_FILE (source), result, &error))
    show_error (self, "The file cannot be deleted", error);
    else
    {
      show_toast (self, "Deleted");
      {
        g_autofree char *directory = NULL;
        g_autofree char *filename = g_path_get_basename (path);

        g_object_get (self->instance, "directory", &directory, NULL);
        aml_content_index_remove_async (directory, self->type, filename,
                                        NULL, NULL, NULL);
      }
      /* An identification request may still refer to one of the rows. Stop
         it before releasing the file it borrowed; the other rows remain in
         place and are not rebuilt. */
      g_cancellable_cancel (self->scan);
      g_clear_object (&self->scan);
      self->scan = g_cancellable_new ();
      self->identifying = FALSE;

      for (guint i = 0; path != NULL && i < self->files->len; i++)
        {
          AmlContentFile *file = g_ptr_array_index (self->files, i);

          if (g_strcmp0 (file->path, path) == 0)
            {
              GtkWidget *row = g_hash_table_lookup (self->rows, path);
              GList *link = g_list_find (self->installed_rows, row);

              if (row != NULL)
                {
                  adw_preferences_group_remove (
                      ADW_PREFERENCES_GROUP (self->installed_group), row);
                  g_hash_table_remove (self->rows, path);
                }

              if (link != NULL)
                {
                  self->installed_rows = g_list_delete_link (
                      self->installed_rows, link);
                }

              g_ptr_array_remove_index (self->files, i);
              gtk_widget_set_visible (self->installed_status,
                                      self->files->len == 0);
              gtk_widget_set_visible (self->installed_scroller,
                                      self->files->len != 0);
              break;
            }
        }
    }

  g_object_unref (self);
}

/* What the confirmation was answered with. The path was copied onto the
 * alert, so a list that is read again while it is up cannot leave it
 * pointing at a file that is gone. */
static void
on_delete_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlContentPage *self = user_data;
  const char *path = g_object_get_data (G_OBJECT (source), "aml-path");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);
  g_autoptr (GFile) target = NULL;

  if (path == NULL || g_strcmp0 (response, "confirm") != 0)
    {
      g_object_unref (self);
      return;
    }

  target = g_file_new_for_path (path);
  g_object_set_data_full (G_OBJECT (target), "aml-path",
                          g_strdup (path), g_free);
  /* Deleting is a system call on the file and goes through GIO off the
     main loop; the ref on the page travels to on_deleted(). */
  g_file_delete_async (target, G_PRIORITY_DEFAULT, NULL, on_deleted, self);
}

/* The trash button asks first, then deletes the file for good. */
static void
on_trash_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlContentFile *file = g_object_get_data (G_OBJECT (button), "aml-file");
  g_autofree char *heading = NULL;
  g_autofree char *body = NULL;
  g_autoptr (GString) dependents = g_string_new (NULL);
  const char *title = NULL;
  AdwAlertDialog *alert = NULL;

  if (file == NULL)
    return;

  title = file->project_id != NULL
            ? g_hash_table_lookup (self->project_titles, file->project_id)
            : NULL;
  if (title == NULL || *title == '\0')
    title = file->filename;

  if (file->project_id != NULL)
    for (guint i = 0; i < self->files->len; i++)
      {
        AmlContentFile *other = g_ptr_array_index (self->files, i);
        const char *other_title = NULL;

        if (other == file || other->required_dependencies == NULL)
          continue;

        for (guint j = 0; j < other->required_dependencies->len; j++)
          if (g_strcmp0 (g_ptr_array_index (other->required_dependencies, j),
                        file->project_id) == 0)
            {
              other_title = other->project_id != NULL
                              ? g_hash_table_lookup (self->project_titles,
                                                     other->project_id)
                              : NULL;
              if (other_title == NULL || *other_title == '\0')
                other_title = other->filename;
              if (dependents->len > 0)
                g_string_append_c (dependents, '\n');
              g_string_append (dependents, other_title);
              break;
            }
      }

  heading = g_strdup_printf (_("Delete \"%s\"?"), title);
  if (dependents->len > 0)
    body = g_strdup_printf (
        _("The following mods depend on %s:\n%s"), title, dependents->str);
  else
    body = NULL;

  alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body));
  if (body == NULL)
    adw_alert_dialog_set_body (alert, "");
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
    adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
    adw_alert_dialog_add_response (alert, "confirm", _("Delete"));
  adw_alert_dialog_set_response_appearance (alert, "confirm",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  g_object_set_data_full (G_OBJECT (alert), "aml-path",
                          g_strdup (file->path), g_free);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_delete_chosen,
                           g_object_ref (self));
}

static GtkWidget *
file_row (AmlContentPage *self, AmlContentFile *file)
{
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *icon = aml_icon_widget_new (aml_content_type_icon (self->type));

  /* A file name and a path are plain text: markup off first, because
     either may hold an "&". Where the file lives is the row's tooltip;
     under the name goes what the project is, once Modrinth says so. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), file->filename);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), FALSE);

  /* The same square as a browse row, with the icon of what this kind of
     file is until the project it belongs to is known; the icon the
     browser saw for that project replaces it then. */
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
  g_object_set_data (G_OBJECT (row), "aml-icon", icon);

  /* Only a mod is something that can be turned off: a resource pack or a
     shader pack is loaded whenever it is present, so neither has a
     switch. */
  if (self->type == AML_CONTENT_MOD)
    {
      GtkWidget *sw = gtk_switch_new ();

      gtk_switch_set_active (GTK_SWITCH (sw), file->enabled);
      gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (sw,
                                   file->enabled ? "Disable" : "Enable");
      gtk_accessible_update_property (GTK_ACCESSIBLE (sw),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Enabled", -1);
      g_object_set_data (G_OBJECT (sw), "aml-file", file);
      g_signal_connect (sw, "state-set", G_CALLBACK (on_mod_toggled), self);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), sw);
    }

  /* Every kind of file can be deleted. On a mod the button sits after
     the switch, at the right end of the row. */
  {
    GtkWidget *trash = gtk_button_new_from_icon_name ("user-trash-symbolic");

    gtk_widget_add_css_class (trash, "flat");
    gtk_widget_set_valign (trash, GTK_ALIGN_CENTER);
   gtk_widget_set_tooltip_text (trash, _("Delete"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (trash),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Delete", -1);
    g_object_set_data (G_OBJECT (trash), "aml-file", file);
    g_signal_connect (trash, "clicked", G_CALLBACK (on_trash_clicked), self);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), trash);
  }

  /* The row carries the file it is about, so the switch does not have to
     know where the file came from. */
  g_object_set_data (G_OBJECT (row), "aml-file", file);
  g_object_set_data (G_OBJECT (row), "aml-page", self);

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
      {
        const char *known_name = g_hash_table_lookup (self->project_titles,
                                                      version->project_id);
        file->project_name = g_strdup (known_name != NULL
                                         ? known_name : version->project_id);
      }
      file->version_id = g_strdup (version->version_id);
      file->version_name = g_strdup (version->name);
      file->version_number = g_strdup (version->version_number);
      {
        AmlModrinthFile *primary =
          aml_modrinth_version_primary_file (version);

        g_free (file->sha512);
        g_free (file->download_url);
        file->sha512 = primary != NULL ? g_strdup (primary->sha512) : NULL;
        file->download_url = primary != NULL ? g_strdup (primary->url) : NULL;
      }
      g_clear_pointer (&file->required_dependencies, g_ptr_array_unref);
      g_clear_pointer (&file->optional_dependencies, g_ptr_array_unref);
      g_clear_pointer (&file->incompatible_dependencies, g_ptr_array_unref);
      file->required_dependencies = g_ptr_array_new_with_free_func (g_free);
      file->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
      file->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);

      for (guint i = 0; version->dependencies != NULL &&
                       i < version->dependencies->len; i++)
        {
          AmlModrinthDependency *dependency =
            g_ptr_array_index (version->dependencies, i);

          if (dependency->project_id == NULL)
            continue;
          if (dependency->type == AML_DEPENDENCY_REQUIRED)
            g_ptr_array_add (file->required_dependencies,
                             g_strdup (dependency->project_id));
          else if (dependency->type == AML_DEPENDENCY_OPTIONAL)
            g_ptr_array_add (file->optional_dependencies,
                             g_strdup (dependency->project_id));
          else if (dependency->type == AML_DEPENDENCY_INCOMPATIBLE)
            g_ptr_array_add (file->incompatible_dependencies,
                             g_strdup (dependency->project_id));
        }

      {
        g_autofree char *directory = NULL;
        g_autofree char *filename = g_path_get_basename (file->path);
        AmlModrinthFile *primary =
          aml_modrinth_version_primary_file (version);
        AmlContentIndexEntry entry = { 0 };

        g_object_get (self->instance, "directory", &directory, NULL);
        entry.filename = filename;
        entry.hash = file->sha1;
        entry.hash512 = primary != NULL ? primary->sha512 : NULL;
        entry.download_url = primary != NULL ? primary->url : NULL;
        entry.env_client = "required";
        entry.env_server = self->type == AML_CONTENT_MOD ? "required"
                                                          : "unsupported";
        entry.provider = "Modrinth";
        entry.project_id = file->project_id;
        entry.project_name = file->project_name;
        entry.icon_url = file->icon_url;
        entry.version_id = file->version_id;
        entry.version_name = file->version_name;
        entry.required_dependencies = file->required_dependencies;
        entry.optional_dependencies = file->optional_dependencies;
        entry.incompatible_dependencies = file->incompatible_dependencies;
        aml_content_index_update_async (directory, self->type, &entry,
                                        run->cancellable, NULL, NULL);
      }

      if (file->project_id != NULL && *file->project_id != '\0')
        g_hash_table_add (self->projects, g_strdup (file->project_id));

      file_row_refresh (row);

      /* The browser's icon of the project the file belongs to, which the
         row carries from here on. */
      installed_icon_lookup (self, row, file->project_id);
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

      /* A hash-matched index entry already identifies this file. The row is
       * ready from disk and reopening an instance must not ask Modrinth just
       * to rediscover what the index already records. */
      if (row == NULL || file->sha1 == NULL ||
          ((file->sha512 != NULL && file->download_url != NULL) &&
           file->project_id != NULL && file->project_name != NULL &&
           *file->project_name != '\0' &&
           g_strcmp0 (file->project_id, file->project_name) != 0))
        continue;

      run->file = file;
      run->row = row;
      aml_modrinth_version_by_hash_async (file->sha1, run->cancellable,
                                          identify_ready, run);
      return;
    }

  self->identifying = FALSE;
  if (self->details_versions != NULL && self->details_download != NULL)
    {
      gboolean have_versions = self->details_versions->len > 0;

     gtk_widget_set_sensitive (self->details_download,
                               have_versions && !self->identifying);
     gtk_widget_set_sensitive (self->details_check,
                               have_versions && !self->identifying);
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

      if (file->project_id != NULL && file->project_name != NULL &&
          *file->project_name != '\0')
        g_hash_table_replace (self->project_titles,
                              g_strdup (file->project_id),
                              g_strdup (file->project_name));
      GtkWidget *row = file_row (self, file);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->installed_group),
                                  row);
      self->installed_rows = g_list_append (self->installed_rows, row);
      g_hash_table_insert (self->rows, g_strdup (file->path), row);
      if (file->project_id != NULL)
        installed_icon_lookup (self, row, file->project_id);
    }

  installed_filter_rows (self);

  {
    gboolean empty = self->files->len == 0;

    gtk_widget_set_visible (self->installed_status, empty);
    gtk_widget_set_visible (self->installed_scroller, !empty);
    gtk_widget_set_sensitive (self->update_all, !empty);
  }

  {
    IdentifyRun *run = g_new0 (IdentifyRun, 1);

    self->identifying = TRUE;
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

/* Update All: check every file Modrinth knows, ask once with the whole
 * list of what is newer, then install that list. Nothing is downloaded
 * before the list is confirmed. */

static void
update_item_free (UpdateItem *item)
{
  if (item == NULL)
    return;

  aml_modrinth_version_free (item->version);
  g_free (item->title);
  g_free (item->path);
  g_free (item);
}

/* Starts the next one of a confirmed batch. When the last one is in, the
 * list is read again and the batch is over. */
static void
update_batch_next (AmlContentPage *self)
{
  UpdateItem *item = NULL;

  if (self->updates == NULL || self->update_index >= self->updates->len)
    {
      guint done = self->updates != NULL ? self->updates->len : 0;

      g_clear_pointer (&self->updates, g_ptr_array_unref);
      self->update_index = 0;
      self->updating = FALSE;
      set_busy (self, FALSE);

      if (done > 0)
        {
          g_autofree char *message = g_strdup_printf (
            "Updated %u %s", done, done == 1 ? "item" : "items");

          show_toast (self, message);
          reload_installed (self);
        }

      return;
    }

  item = g_ptr_array_index (self->updates, self->update_index);
  self->update_index++;

  /* The batch was confirmed once, as a whole: this plan is not asked for
     again, it is simply carried out. */
  set_busy (self, TRUE);
  start_plan (self, aml_modrinth_version_copy (item->version), item->title,
              item->path, item->disabled);
}

static void
plan_stopped (AmlContentPage *self)
{
  if (self->updating)
    update_batch_next (self);
  else if (self->picking)
    batch_next (self);
  else
    set_busy (self, FALSE);
}

/* What the confirmation was answered with. */
static void
on_update_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlContentPage *self = user_data;
  UpdateRun *run = g_object_get_data (G_OBJECT (source), "aml-update-run");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (run == NULL || self == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (g_strcmp0 (response, "update") != 0)
    {
      g_clear_pointer (&run->items, g_ptr_array_unref);
      g_free (run);
      g_object_unref (self);
      return;
    }

  /* The whole list was confirmed: install each one. */
  self->updates = g_steal_pointer (&run->items);
  self->update_index = 0;
  self->updating = TRUE;
  g_free (run);
  update_batch_next (self);

  g_object_unref (self);
}

/* The check is over: what is newer is asked for in one question, and an
 * empty answer is said as such. */
static void
update_check_done (AmlContentPage *self, UpdateRun *run)
{
  g_object_set_data (G_OBJECT (self), "aml-update-run", NULL);
  set_busy (self, FALSE);

  if (run->items->len == 0)
    {
      show_toast (self, "Everything is up to date");
      g_clear_pointer (&run->items, g_ptr_array_unref);
      g_free (run);
      return;
    }

  {
    const char *what = self->type == AML_CONTENT_MOD
                         ? "mods"
                         : self->type == AML_CONTENT_RESOURCE_PACK
                             ? "resource packs"
                             : "shader packs";
    g_autofree char *heading =
      g_strdup_printf (_("Update the following %s"), what);
    g_autoptr (GString) body = g_string_new (NULL);
    AdwAlertDialog *alert = NULL;

    for (guint i = 0; i < run->items->len; i++)
      {
        UpdateItem *item = g_ptr_array_index (run->items, i);

        if (i > 0)
          g_string_append_c (body, '\n');

        g_string_append (body, item->title);
      }

    alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body->str));
    adw_alert_dialog_set_heading_use_markup (alert, FALSE);
    adw_alert_dialog_set_body_use_markup (alert, FALSE);
    adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
    adw_alert_dialog_add_response (alert, "update", _("Confirm"));
    adw_alert_dialog_set_response_appearance (alert, "update",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (alert, "cancel");
    adw_alert_dialog_set_close_response (alert, "cancel");
    g_object_set_data (G_OBJECT (alert), "aml-update-run", run);
    adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_update_chosen,
                             g_object_ref (self));
  }
}

/* The newer version Modrinth has for one file, or nothing when there is
 * none. Either way the check goes on to the next file. */
static void
on_update_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  PageWatch *watch = user_data;
  AmlContentPage *self = watch->page;
  g_autoptr (GError) error = NULL;
  AmlModrinthVersion *update = NULL;
  AmlContentFile *file = NULL;
  UpdateRun *run = NULL;

  if (self == NULL)
    {
      page_watch_free (watch);
      return;
    }

  update = aml_modrinth_update_by_hash_finish (result, &error);
  file = g_object_get_data (G_OBJECT (self), "aml-update-file");
  run = g_object_get_data (G_OBJECT (self), "aml-update-run");

  if (run == NULL)
    {
      aml_modrinth_version_free (update);
      page_watch_free (watch);
      return;
    }

  /* The update endpoint answers with the newest version that fits, which
     is still the installed one when the file is already the newest: a
     version that carries the very file on disk is not an update. */
  if (update != NULL && file != NULL &&
      !aml_modrinth_version_has_hash (update, file->sha1))
    {
      UpdateItem *item = g_new0 (UpdateItem, 1);

      item->version = update; /* owned from here */
       {
         const char *title = file->project_id != NULL
                               ? g_hash_table_lookup (self->project_titles,
                                                      file->project_id)
                               : NULL;

         /* Update dialogs use the project name, never the version label.
          * A filename is the safest fallback while its project metadata is
          * still being resolved. */
         item->title = g_strdup (title != NULL ? title : file->filename);
       }
      item->path = g_strdup (file->path);
      item->disabled = !file->enabled;
      g_ptr_array_add (run->items, item);
    }
  else
    {
      /* Nothing newer, or the question could not be answered: neither is
         an update to install. */
      aml_modrinth_version_free (update);
      g_clear_error (&error);
    }

  update_check_next (run);
  page_watch_free (watch);
}

/* One file after another, so a folder of twenty does not open twenty
 * requests at once. */
static void
update_check_next (UpdateRun *run)
{
  AmlContentPage *self = run != NULL ? run->page : NULL;

  if (self == NULL)
    {
      g_clear_pointer (&run->items, g_ptr_array_unref);
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

  update_check_done (self, run);
}

static void
on_update_all_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  UpdateRun *run = g_new0 (UpdateRun, 1);

  run->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
  run->items =
    g_ptr_array_new_with_free_func ((GDestroyNotify) update_item_free);
  g_object_set_data (G_OBJECT (self), "aml-update-run", run);
  set_busy (self, TRUE);
  update_check_next (run);
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
installed_filter_rows (AmlContentPage *self);

/* ---------------------------------------------------------- *
 * Browse
 * ---------------------------------------------------------- */

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

/* An installed file that Modrinth knows: the icon the browser showed for
 * its project is what the row should carry too. The url is remembered
 * from the browser when it was seen there, and a project that was not is
 * asked Modrinth for once. */
typedef struct {
  PageWatch *watch;   /* the page, weakly held */
  GtkWidget *row;     /* the row whose icon is wanted, held for the wait */
  char *project_id;
} InstalledIcon;

static void
installed_icon_free (InstalledIcon *lookup)
{
  page_watch_free (lookup->watch);
  g_object_unref (lookup->row);
  g_free (lookup->project_id);
  g_free (lookup);
}

static void
repair_index_for_file (AmlContentPage *self, AmlContentFile *file)
{
  g_autofree char *directory = NULL;
  g_autofree char *filename = g_path_get_basename (file->path);
  AmlContentIndexEntry entry = { 0 };

  g_object_get (self->instance, "directory", &directory, NULL);
  entry.filename = filename;
  entry.hash = file->sha1;
  entry.provider = "Modrinth";
  entry.project_id = file->project_id;
  entry.project_name = file->project_name;
  entry.icon_url = file->icon_url;
  entry.version_id = file->version_id;
  entry.version_name = file->version_name;
  entry.required_dependencies = file->required_dependencies;
  entry.optional_dependencies = file->optional_dependencies;
  entry.incompatible_dependencies = file->incompatible_dependencies;
  aml_content_index_update_async (directory, self->type, &entry, self->scan,
                                  NULL, NULL);
}

static void
installed_icon_details (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                        gpointer user_data)
{
  InstalledIcon *lookup = user_data;
  AmlContentPage *self = lookup->watch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlModrinthDetails) details = NULL;

  details = aml_modrinth_details_finish (result, &error);

  if (self != NULL && details != NULL)
    {
      GtkWidget *icon = NULL;
      AmlContentFile *file = g_object_get_data (G_OBJECT (lookup->row),
                                                "aml-file");

      if (details->title != NULL && *details->title != '\0')
        {
          g_hash_table_insert (self->project_titles,
                               g_strdup (lookup->project_id),
                               g_strdup (details->title));
          if (file != NULL)
            {
              g_free (file->project_name);
              file->project_name = g_strdup (details->title);
            }
        }
      file_row_refresh (lookup->row);

      /* What the project is, under the file's name, and the icon. Both
         are remembered for the rows of the same project, rebuilt or
         reached through the browser. */
      if (details->description != NULL && *details->description != '\0')
        {
          g_hash_table_insert (self->project_descriptions,
                               g_strdup (lookup->project_id),
                               g_strdup (details->description));
          row_show_description (lookup->row, details->description);
        }

      if (details->icon_url != NULL && *details->icon_url != '\0')
        {
          g_hash_table_insert (self->project_icons,
                               g_strdup (lookup->project_id),
                              g_strdup (details->icon_url));

          if (file != NULL)
            {
              g_free (file->icon_url);
              file->icon_url = g_strdup (details->icon_url);
            }

          icon = g_object_get_data (G_OBJECT (lookup->row), "aml-icon");

          if (icon != NULL)
            start_row_icon (icon, details->icon_url);
        }
      if (file != NULL)
        repair_index_for_file (self, file);
    }

  installed_icon_free (lookup);
}

static void
installed_icon_lookup (AmlContentPage *self, GtkWidget *row,
                       const char *project_id)
{
  AmlContentFile *file = g_object_get_data (G_OBJECT (row), "aml-file");
  const char *known = NULL;
  const char *description = NULL;

  if (project_id == NULL || *project_id == '\0')
    return;

  if (file != NULL && file->icon_url != NULL && *file->icon_url != '\0')
    {
      GtkWidget *icon = g_object_get_data (G_OBJECT (row), "aml-icon");
      if (icon != NULL)
        start_row_icon (icon, file->icon_url);
      return;
    }

  known = g_hash_table_lookup (self->project_icons, project_id);
  description = g_hash_table_lookup (self->project_descriptions, project_id);

  /* A description the browser already read goes in without asking again,
     even when the icon still has to be fetched. */
  if (description != NULL)
    row_show_description (row, description);

  if (known != NULL)
    {
      GtkWidget *icon = g_object_get_data (G_OBJECT (row), "aml-icon");

      if (icon != NULL)
        start_row_icon (icon, known);

      return;
    }

  {
    InstalledIcon *lookup = g_new0 (InstalledIcon, 1);

    lookup->watch = page_watch_new (self);
    lookup->row = g_object_ref (row);
    lookup->project_id = g_strdup (project_id);

    aml_modrinth_details_async (project_id, self->scan, installed_icon_details,
                                lookup);
  }
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
      GtkWidget *row = NULL;
      GtkWidget *icon = NULL;

      /* A project already listed is not listed twice: the same answer can
       * come back for two pages that overlap. */
      if (g_hash_table_contains (self->seen_ids, project->project_id))
        continue;

      row = adw_action_row_new ();
      icon = aml_icon_widget_new (aml_content_type_icon (self->type));

      g_hash_table_add (self->seen_ids, g_strdup (project->project_id));

      /* What the browser saw, remembered for the installed rows of the
         same project. */
      if (project->icon_url != NULL && *project->icon_url != '\0')
        g_hash_table_insert (self->project_icons,
                             g_strdup (project->project_id),
                             g_strdup (project->icon_url));

      if (project->description != NULL && *project->description != '\0')
        g_hash_table_insert (self->project_descriptions,
                             g_strdup (project->project_id),
                             g_strdup (project->description));

      if (project->title != NULL && *project->title != '\0')
        g_hash_table_insert (self->project_titles,
                             g_strdup (project->project_id),
                             g_strdup (project->title));

      /* The icon, then the name and the description under it. Nothing
         else: who wrote it, how often it was taken, which game version
         and which loader are what the details page is for. Markup is
         turned off before the title and the description go in, because
         both are plain text from Modrinth and may hold an "&". */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                     project->title);
      {
        g_autofree char *line = description_line (project->description, 0);

        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), line);
      }
      /* Two lines at most, ending in an ellipsis when the description is
         longer: every row is then the same height, whatever it holds.
         The whole description is on the details page. */
      adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
      adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), FALSE);

      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      start_row_icon (icon, project->icon_url);

      /* The rows point into the list the page owns, so the projects
       * live exactly as long as the rows that show them. */
      g_ptr_array_add (self->results, aml_modrinth_project_copy (project));
      g_object_set_data (G_OBJECT (row), "aml-project",
                         g_ptr_array_index (self->results,
                                            self->results->len - 1));

      adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);

      /* The whole row opens the project, where the version is chosen.
       * An AdwActionRow is not activatable by default, so the row
       * itself is made one: a click on it activates the row. */
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_signal_connect (row, "activated", G_CALLBACK (on_result_activated),
                        self);

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
    gtk_widget_set_visible (self->results_group, TRUE);
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

static GtkWidget *
active_search_entry (AmlContentPage *self)
{
  const char *visible = self->stack != NULL
    ? adw_view_stack_get_visible_child_name (ADW_VIEW_STACK (self->stack))
    : NULL;

  return g_strcmp0 (visible, "browse") == 0
           ? self->search_entry : self->installed_search_entry;
}

void
aml_content_page_focus_search (AmlContentPage *self)
{
  GtkWidget *entry;

  g_return_if_fail (AML_IS_CONTENT_PAGE (self));

  entry = active_search_entry (self);
  if (entry != NULL)
    {
      gtk_widget_set_visible (entry, TRUE);
      gtk_widget_grab_focus (entry);
    }
}

static void
on_content_search_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  GtkWidget *entry = active_search_entry (self);

  if (entry != NULL)
    {
      gtk_widget_set_visible (entry, TRUE);
      gtk_widget_grab_focus (entry);
    }
}

static gboolean
on_content_search_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
                       guint keyval, guint keycode G_GNUC_UNUSED,
                       GdkModifierType state, gpointer user_data)
{
  AmlContentPage *self = user_data;
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
            gtk_widget_set_margin_top (heading, 6);
            gtk_widget_set_margin_start (heading, 6);
            gtk_box_append (GTK_BOX (self->filter_box), heading);
          }

        {
          g_autofree char *label_text =
            aml_modrinth_category_label (category->name);

          button = gtk_check_button_new_with_label (label_text);
        }

        gtk_widget_add_css_class (button, "flat");
        /* The whole width of the popover is the target, so a category is
           not a small checkbox at the left that has to be aimed at. */
        gtk_widget_set_hexpand (button, TRUE);
        gtk_widget_set_halign (button, GTK_ALIGN_FILL);
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
        GtkWidget *retry = gtk_button_new_with_label (_("Retry"));

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

/* ---------------------------------------------------------- *
 * The page of one project
 * ---------------------------------------------------------- */



/* What the download button would take: the version the dropdown shows,
 * or NULL when none is chosen yet. */
static AmlModrinthVersion *
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
static const char *
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
              self->current_project_title != NULL
                ? self->current_project_title : version_name_of (version),
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
static void
close_detail (AmlContentPage *self)
{
  if (self->split == NULL)
    return;

  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), NULL);
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), TRUE);
  watch_widget (&self->details_page, NULL);
  g_clear_pointer (&self->current_project_id, g_free);
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
  title = gtk_label_new (self->current_project_title);
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_add_css_class (title, "title-2");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), title);

  {
    const char *who = self->details_author != NULL &&
                      *self->details_author != '\0'
                        ? self->details_author
                        : "";
    g_autofree char *text = NULL;

    if (self->details != NULL && self->details->license != NULL &&
        *self->details->license != '\0')
      text = g_strdup_printf ("%s  ·  by %s", self->details->license, who);
    else
      text = g_strdup_printf ("by %s", who);

    meta = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (meta), 0);
    gtk_widget_add_css_class (meta, "dim-label");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), meta);
  }

  downloads = format_downloads (self->current_project_downloads);
  count = gtk_label_new (downloads);
  gtk_label_set_xalign (GTK_LABEL (count), 0);
  gtk_widget_add_css_class (count, "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (heading), count);

  /* The long description, in Markdown: headings, links and images are
     read and shown as widgets rather than as the raw text. */
  watch_widget (&self->details_body, gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));

  if (self->details_problem != NULL)
    gtk_box_append (GTK_BOX (self->details_body),
                    plain_label (self->details_problem));
  else if (self->details != NULL && self->details->body != NULL &&
           *self->details->body != '\0')
    aml_markdown_render (GTK_BOX (self->details_body), self->details->body);
  else if (self->details != NULL)
    gtk_box_append (GTK_BOX (self->details_body),
                    plain_label (self->details->description));

  gtk_box_append (GTK_BOX (box), heading);
  gtk_box_append (GTK_BOX (box), self->details_body);

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
  watch_widget (&self->details_check, gtk_check_button_new ());
  gtk_widget_set_valign (self->details_check, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (self->details_check, FALSE);
  gtk_widget_set_tooltip_text (self->details_check,
                               _("Install this version with the selected "
                                 "ones"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->details_check),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Install this version with the selected "
                                    "ones"), -1);
  g_signal_connect (self->details_check, "toggled",
                    G_CALLBACK (on_version_picked), self);

  watch_widget (&self->details_dropdown, gtk_drop_down_new (NULL, NULL));
  {
    GtkListItemFactory *factory = version_factory ();

    gtk_drop_down_set_factory (GTK_DROP_DOWN (self->details_dropdown), factory);
    gtk_drop_down_set_list_factory (GTK_DROP_DOWN (self->details_dropdown),
                                    factory);
    g_object_unref (factory);
  }
  g_signal_connect (self->details_dropdown, "notify::selected",
                    G_CALLBACK (on_version_selected), self);
  gtk_widget_set_hexpand (self->details_dropdown, TRUE);
  gtk_widget_set_valign (self->details_dropdown, GTK_ALIGN_CENTER);

  watch_widget (&self->details_download, gtk_button_new_with_label (_("Download")));
  gtk_widget_add_css_class (self->details_download, "suggested-action");
  gtk_widget_set_valign (self->details_download, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (self->details_download, FALSE);
  g_signal_connect (self->details_download, "clicked",
                    G_CALLBACK (on_version_download_clicked), self);

  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (bar), self->details_check);
  gtk_box_append (GTK_BOX (bar), self->details_dropdown);
  gtk_box_append (GTK_BOX (bar), self->details_download);
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
  if (self == NULL || self->details_page == NULL)
    return;

  if (!self->details_ready || !self->versions_ready)
    return;

  adw_navigation_page_set_child (ADW_NAVIGATION_PAGE (self->details_page),
                                 details_content (self));

  if (self->versions_problem != NULL)
    show_toast (self, self->versions_problem);
  else if (self->details_versions != NULL && self->details_versions->len == 0)
    show_toast (self, "This project publishes nothing that fits this instance.");

  if (self->details_versions != NULL && self->details_versions->len > 0)
    {
      GtkStringList *labels = gtk_string_list_new (NULL);

      for (guint i = 0; i < self->details_versions->len; i++)
        {
          g_autofree char *label =
            version_label (g_ptr_array_index (self->details_versions, i));

          gtk_string_list_append (labels, label);
        }

      gtk_drop_down_set_model (GTK_DROP_DOWN (self->details_dropdown),
                               G_LIST_MODEL (labels));
      /* The newest version is what a user most often wants, so it is
         what the dropdown shows from the start. */
      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->details_dropdown), 0);
      g_object_unref (labels);

      /* The button owns the versions from here, so the install it starts
         does not depend on the page still holding them. */
      g_object_set_data_full (G_OBJECT (self->details_download), "aml-versions",
                              g_ptr_array_ref (self->details_versions),
                              (GDestroyNotify) g_ptr_array_unref);
      g_object_set_data (G_OBJECT (self->details_download), "aml-dropdown",
                         self->details_dropdown);
    }

  {
    gboolean have_versions = self->details_versions != NULL &&
                             self->details_versions->len > 0;

    gtk_widget_set_sensitive (self->details_download,
                              have_versions && !self->identifying);
    /* Nothing to put on the list while the dropdown shows none. */
    gtk_widget_set_sensitive (self->details_check,
                              have_versions && !self->identifying);
  }

  /* The version the dropdown shows from the start is not on the list
     until the checkmark says so. */
  refresh_picks (self);
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
  if (g_strcmp0 (self->current_project_id, watch->project_id) != 0)
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
        self->versions_problem = g_strdup (error->message);
      else
        self->versions_problem = g_strdup ("no compatible version");
    }
  else
    {
      self->details_versions = g_steal_pointer (&versions);
    }

  self->versions_ready = TRUE;
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
  if (g_strcmp0 (self->current_project_id, watch->project_id) != 0)
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
      if (self->details_problem == NULL)
        self->details_problem =
          g_strdup (error != NULL ? error->message
                                  : "the project cannot be read");
    }
  else
    {
      g_clear_pointer (&self->details, aml_modrinth_details_free);
      self->details = g_steal_pointer (&details);
    }

  self->details_ready = TRUE;
  detail_fill (self);

  /* Only versions that fit this instance are asked for, and only those
     can reach the list. The watch goes on to the next answer, which is
     the one that frees it. */
  aml_modrinth_versions_async (watch->project_id, self->type,
                               self->minecraft_version, self->loader,
                               self->browse, on_detail_versions_ready, watch);
}

/* The dialog of @project: what it is, every version of it that fits this
 * instance, and the one button that installs the version chosen from
 * them. It is opened by activating a result row, so the version is
 * always chosen by the user and never picked unseen. The long
 * description is Markdown and is shown as such. */
static void
open_project_page (AmlContentPage *self, AmlModrinthProject *project)
{
  GtkWidget *page = NULL;

  if (project == NULL)
    return;

  /* What the page needs to rebuild its heading once the answers are in.
     The page itself is built with a spinner and filled later. The id is
     kept so a second click on the same row closes it again. */
  g_free (self->current_project_title);
  self->current_project_title = g_strdup (project->title);
  g_free (self->current_project_id);
  self->current_project_id = g_strdup (project->project_id);
  self->current_project_downloads = project->downloads;
  g_free (self->details_author);
  self->details_author = g_strdup (project->author);

  self->details_ready = FALSE;
  self->versions_ready = FALSE;
  g_clear_pointer (&self->details, aml_modrinth_details_free);
  g_clear_pointer (&self->details_versions, g_ptr_array_unref);
  g_free (self->details_problem);
  self->details_problem = NULL;
  g_free (self->versions_problem);
  self->versions_problem = NULL;

  /* The project opens in the right half of the page, beside the list it
     was picked from, showing a spinner until both the description and
     the versions are in. Its name is the first thing the page shows. */
  page = GTK_WIDGET (adw_navigation_page_new (loading_child (),
                                              project->title));
  watch_widget (&self->details_page, page);

  adw_navigation_split_view_set_content (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), ADW_NAVIGATION_PAGE (page));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), FALSE);

  set_busy (self, TRUE);
  aml_modrinth_details_async (project->project_id, self->browse,
                              on_details_ready,
                              details_watch_new (self, project->project_id));
}

/* A click on a row opens its project in the other half. A click on the
 * row of the project that is already open closes it again: the list is
 * the way in and the way out, so no button has to stand over the project
 * and push it down. */
static void
on_result_activated (AdwActionRow *row, gpointer user_data)
{
  AmlContentPage *self = user_data;
  AmlModrinthProject *project =
    g_object_get_data (G_OBJECT (row), "aml-project");

  if (project == NULL)
    return;

  if (self->details_page != NULL &&
      g_strcmp0 (self->current_project_id, project->project_id) == 0)
    {
      close_detail (self);
      return;
    }

  open_project_page (self, project);
}

/* ---------------------------------------------------------- *
 * Building the page
 * ---------------------------------------------------------- */

static void
build_installed (AmlContentPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *search_bar = gtk_search_entry_new ();
  GtkWidget *search_button = toolbar_button (
    "system-search-symbolic", _("Search"),
    G_CALLBACK (on_content_search_clicked), self);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  AdwPreferencesGroup *group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

   gtk_widget_set_hexpand (actions, TRUE);
   self->installed_search_bar = search_bar;
   self->installed_search_entry = search_bar;
   gtk_widget_set_hexpand (search_bar, TRUE);
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_bar),
                                            _("Search"));
    gtk_widget_set_visible (search_bar, FALSE);
   g_signal_connect (search_bar, "changed",
                     G_CALLBACK (on_installed_search_changed), self);
    gtk_box_append (GTK_BOX (actions), search_button);
    gtk_box_append (GTK_BOX (actions), search_bar);

   self->update_all = toolbar_button ("software-update-available-symbolic", _("Update All"),
                                      G_CALLBACK (on_update_all_clicked), self);
  gtk_widget_set_sensitive (self->update_all, FALSE);
  gtk_box_append (GTK_BOX (actions), self->update_all);
  gtk_box_append (GTK_BOX (actions),
                  toolbar_button ("folder-symbolic", _("Open Folder"),
                                  G_CALLBACK (on_open_folder_clicked), self));
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
                               _("Nothing installed yet"));
  adw_status_page_set_description (
      ADW_STATUS_PAGE (self->installed_status),
       _("Browse for something to put into the folder of this instance"));

  /* The list starts where the row above it ends: its first row is not
     rounded at the top, so the two read as one piece. */
  gtk_widget_add_css_class (GTK_WIDGET (group), "flush-top");

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
                                        "installed", _("Installed"),
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

  /* The switcher follows the list, however the list was changed: a page
     that is built again keeps the button of the page on top on. */
  if (self->switcher_installed != NULL && self->switcher_browse != NULL)
    gtk_toggle_button_set_active (
      GTK_TOGGLE_BUTTON (browsing ? self->switcher_browse
                                  : self->switcher_installed), TRUE);

  /* The row shows the actions of whichever list is on top. */
  move_actions_into_bar (self, browsing);

  /* Installed is a list of its own: the project page opened from Browse
     is left with the list it was picked from. */
  if (!browsing)
    {
      clear_picks (self);
      close_detail (self);
    }

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
  GtkWidget *search_bar = gtk_search_entry_new ();
  GtkWidget *search_button = toolbar_button (
    "system-search-symbolic", _("Search"),
    G_CALLBACK (on_content_search_clicked), self);
  GtkWidget *filter_button = gtk_menu_button_new ();
  GtkWidget *popover = gtk_popover_new ();
  GtkWidget *filter_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *filter_scroller = gtk_scrolled_window_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *loading_more = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *loading_spinner = gtk_spinner_new ();
  GtkWidget *loading_label = gtk_label_new (_("Loading more…"));
  GtkWidget *retry_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *retry_label = gtk_label_new (NULL);
  GtkWidget *retry_button = gtk_button_new_with_label (_("Retry"));
  GtkWidget *foot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *pick_button = gtk_button_new ();
  GtkWidget *pick_icon = gtk_image_new_from_icon_name (
      "folder-download-symbolic");
  GtkWidget *pick_overlay = gtk_overlay_new ();
  GtkWidget *pick_badge = gtk_label_new (NULL);
  g_autofree char *pick_accessible_label = NULL;
  AdwPreferencesGroup *group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  self->search_entry = search_bar;
  self->search_bar = search_bar;
  /* The field takes the room the row has, the way the modpack browser's
     search does, rather than a fixed width of its own. */
  gtk_widget_set_hexpand (search_bar, TRUE);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_bar),
                                            _("Search"));
  gtk_widget_set_visible (search_bar, FALSE);

  g_signal_connect (search_bar, "changed",
                    G_CALLBACK (on_search_changed), self);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_BUBBLE);
    gtk_widget_add_controller (search_bar, keys);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_search_key), self);
  }

  gtk_box_append (GTK_BOX (top), search_button);
  gtk_box_append (GTK_BOX (top), search_bar);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (filter_button),
                                 "view-more-symbolic");
    gtk_widget_set_tooltip_text (filter_button, _("Filter by category"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (filter_button),
                                     GTK_ACCESSIBLE_PROPERTY_LABEL, _("Filter"),
                                  -1);
  gtk_widget_set_valign (filter_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (filter_button, "flat");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (filter_button), popover);
  /* A click opens the popover but does not emit "activate" on the
     menu button, so the load hangs off the popover's "show" instead. */
  g_signal_connect (popover, "show",
                    G_CALLBACK (on_filter_clicked), self);
  gtk_box_append (GTK_BOX (top), filter_button);

  /* The row takes the room the bar has, the search inside it takes most
     of that, and the filter and the switcher sit at its right. */
  gtk_widget_set_hexpand (top, TRUE);

  self->filter_box = filter_box;
  gtk_widget_set_margin_top (filter_box, 6);
  gtk_widget_set_margin_bottom (filter_box, 6);
  gtk_widget_set_margin_start (filter_box, 6);
  gtk_widget_set_margin_end (filter_box, 6);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (filter_scroller),
                                 filter_box);
  /* Wide enough to read a category and its heading, and no taller than
     the popover can be: a long list scrolls instead of running off the
     screen. */
  gtk_widget_set_size_request (filter_scroller, 280, -1);
  gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (filter_scroller), 420);
  gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (filter_scroller), TRUE);
  gtk_popover_set_child (GTK_POPOVER (popover), filter_scroller);

  self->browse_actions = top;
  g_object_ref_sink (top);

  self->results_status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->results_status),
                                 "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->results_status),
                               _("Nothing to show yet"));
  adw_status_page_set_description (
      ADW_STATUS_PAGE (self->results_status),
        _("Type what to look for, or browse everything Modrinth has for this "
        "instance"));
  /* The list starts where the row above it ends: its first row is not
     rounded at the top, so the two read as one piece. */
  gtk_widget_add_css_class (GTK_WIDGET (group), "flush-top");

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

  /* The foot of the list: the button that installs everything the
     checkmarks of the versions have on, and nothing else. It is a plain
     download icon at the left end of the row, where the actions of the
     other list sit. */
  gtk_widget_add_css_class (pick_button, "flat");
  gtk_widget_set_valign (pick_button, GTK_ALIGN_CENTER);
   pick_accessible_label = g_strdup_printf (
       _("Install the selected %s?"), aml_content_type_title (self->type));
   gtk_accessible_update_property (GTK_ACCESSIBLE (pick_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   pick_accessible_label, -1);
  gtk_button_set_child (GTK_BUTTON (pick_button), pick_icon);
  g_signal_connect (pick_button, "clicked",
                    G_CALLBACK (on_pick_button_clicked), self);

   /* Keep the count attached to the button without putting it inside the
      button's hit target. The overlay keeps it correctly positioned at
      every scale without changing the button's width. */
   gtk_widget_add_css_class (pick_badge, "aml-badge");
   gtk_widget_set_valign (pick_badge, GTK_ALIGN_START);
   gtk_widget_set_halign (pick_badge, GTK_ALIGN_END);
   gtk_widget_set_margin_top (pick_badge, 1);
   gtk_widget_set_margin_end (pick_badge, 1);

   gtk_overlay_set_child (GTK_OVERLAY (pick_overlay), pick_button);
   gtk_overlay_add_overlay (GTK_OVERLAY (pick_overlay), pick_badge);
   gtk_box_append (GTK_BOX (foot), pick_overlay);
  gtk_widget_set_margin_start (foot, 12);
  gtk_widget_set_margin_end (foot, 12);
  gtk_widget_set_margin_top (foot, 6);
  gtk_widget_set_margin_bottom (foot, 6);
  gtk_box_append (GTK_BOX (box), foot);

  self->pick_button = pick_button;
  self->pick_badge = pick_badge;

  /* Nothing is picked yet: the button starts where the list starts, with
     no number on it and nothing to install. */
  refresh_picks (self);

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
                                        "browse", _("Browse"),
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

/* The two buttons that show one of the lists. They carry the icons only:
 * the buttons have no words, so the row they share with the search field
 * stays one line at any width, and the tooltip is what says which list
 * each one shows. */
static void
on_switcher_toggled (GtkToggleButton *button, gpointer user_data)
{
  AmlContentPage *self = user_data;

  if (!gtk_toggle_button_get_active (button) || self->stack == NULL)
    return;

  adw_view_stack_set_visible_child_name (
    ADW_VIEW_STACK (self->stack),
    button == GTK_TOGGLE_BUTTON (self->switcher_browse) ? "browse"
                                                        : "installed");
}

static GtkWidget *
switcher_button (const char *icon_name, const char *label)
{
  GtkWidget *button = gtk_toggle_button_new ();

  gtk_button_set_icon_name (GTK_BUTTON (button), icon_name);
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
     gtk_widget_set_tooltip_text (button, _(label));
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, _(label), -1);

  return button;
}

static void
build_bar (AmlContentPage *self)
{
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  self->bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);
    g_signal_connect (keys, "key-pressed",
                      G_CALLBACK (on_content_search_key), self);
  }

  /* The actions go in before this and the switcher after it, so the
     switcher ends up at the right end of the row however many actions
     there are. Aligning inside a box would not move it: a box gives every
     child its own width. */
  self->bar_spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (self->bar_spacer, TRUE);

  /* Two buttons that behave as one choice, drawn as the linked pair of
     GNOME. A view switcher of libadwaita always carries the words of a
     page under its icon, and this row has no room for two lines. */
  self->switcher = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->switcher, "linked");

  self->switcher_installed =
     switcher_button ("view-list-symbolic", "Installed");
   self->switcher_browse =
     switcher_button ("web-browser-symbolic", "Browse");

  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->switcher_browse),
                               GTK_TOGGLE_BUTTON (self->switcher_installed));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->switcher_installed),
                                TRUE);

  g_signal_connect (self->switcher_installed, "toggled",
                    G_CALLBACK (on_switcher_toggled), self);
  g_signal_connect (self->switcher_browse, "toggled",
                    G_CALLBACK (on_switcher_toggled), self);

  gtk_box_append (GTK_BOX (self->switcher), self->switcher_installed);
  gtk_box_append (GTK_BOX (self->switcher), self->switcher_browse);

  gtk_widget_set_valign (self->switcher, GTK_ALIGN_CENTER);

  gtk_box_append (GTK_BOX (self->bar), self->bar_spacer);
  gtk_box_append (GTK_BOX (self->bar), self->switcher);

  gtk_widget_set_margin_top (self->bar, 6);
  gtk_widget_set_margin_bottom (self->bar, 6);
  gtk_widget_set_margin_start (self->bar, 12);
  gtk_widget_set_margin_end (self->bar, 12);

  /* A failure is said in a banner above the row, where it stays until it
     is read; what a toast says is an operation that just worked. */
  self->banner = adw_banner_new ("");
  adw_banner_set_button_label (ADW_BANNER (self->banner), "Dismiss");
  adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
  g_signal_connect (self->banner, "button-clicked",
                    G_CALLBACK (on_banner_dismiss), self);

  gtk_box_append (GTK_BOX (inner), self->banner);
  gtk_box_append (GTK_BOX (inner), self->bar);
  gtk_box_append (GTK_BOX (inner), self->stack);

  /* The list is the left half; the project that is opened is the right
     half, and is empty until one is opened. Collapsed shows one page at a
     time, which is the list alone. */
  self->split = adw_navigation_split_view_new ();
  gtk_widget_set_vexpand (self->split, TRUE);
  /* Named, so the stylesheet keeps the left half the colour of the page
     instead of the sidebar colour it would take on its own. */
  gtk_widget_add_css_class (self->split, "flat-split");
  adw_navigation_split_view_set_sidebar (
    ADW_NAVIGATION_SPLIT_VIEW (self->split),
    adw_navigation_page_new (inner, aml_content_type_title (self->type)));
  adw_navigation_split_view_set_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), TRUE);
  adw_navigation_split_view_set_sidebar_width_fraction (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), 0.5);
  /* Half the page, rather than the narrow column a split view gives a
     sidebar by default: this half holds the search row and the list. */
  adw_navigation_split_view_set_min_sidebar_width (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), 360);
  adw_navigation_split_view_set_max_sidebar_width (
    ADW_NAVIGATION_SPLIT_VIEW (self->split), 640);

  /* One overlay over the list and the project, so a message about either
     one is said in the same place. */
  self->toasts = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toasts), self->split);
  gtk_box_append (GTK_BOX (self), self->toasts);
}

/* The actions of the list being shown go into the row, at its left, and
   the ones of the other list come out. A widget with no parent is the one
   that belongs here, because only one list holds them at a time.

   Browse brings its own row, whose search field takes the room; nothing
   has to push the switcher to the right for it. Installed has no such
   row, so the spacer does that. */
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

   /* Both action rows contain an expanding search entry. The spacer used by
      the old Installed-only toolbar would consume that room and leave the
      entry at its natural width. */
   if (self->bar_spacer != NULL)
     gtk_widget_set_visible (self->bar_spacer, FALSE);
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
  self->more = g_cancellable_new ();
  self->seen_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          NULL);
  self->categories = g_ptr_array_new_with_free_func (g_free);
  self->projects = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          NULL);
  self->project_icons = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                               g_free);
  self->project_descriptions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                       g_free, g_free);
  self->project_titles = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_free);

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
                                      _("No mod loader"));
          adw_status_page_set_description (
              ADW_STATUS_PAGE (status),
              _("Mods need a mod loader. Pick Fabric, Quilt, Forge or NeoForge "
              "in the settings of this instance first"));
          gtk_box_append (GTK_BOX (self), status);

          return self;
        }
    }

  /* Modrinth reads its categories and loaders in lower case, which is
   * what aml_loader_to_string() writes. */
  g_object_get (instance, "loader", &value, NULL);
  loader = aml_loader_to_string (value);
  self->loader = loader != NULL ? g_strdup (loader) : NULL;

  self->details_ready = FALSE;
  self->versions_ready = FALSE;
  self->details = NULL;
  self->details_versions = NULL;
  self->details_problem = NULL;
  self->versions_problem = NULL;
  self->current_project_title = NULL;
  self->current_project_id = NULL;
  self->current_project_downloads = 0;
  self->details_author = NULL;
  self->details_body = NULL;
  self->details_dropdown = NULL;
  self->details_download = NULL;
  self->identifying = FALSE;
  self->details_check = NULL;
  self->details_page = NULL;
  self->split = NULL;
  self->batch = NULL;
  self->picking = FALSE;
  self->pick_index = 0;
  self->pick_button = NULL;
  self->pick_badge = NULL;

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
  self->banner = NULL;
  self->stack = NULL;
  self->split = NULL;
  self->bar = NULL;
  self->bar_spacer = NULL;
  self->switcher = NULL;
  self->switcher_installed = NULL;
  self->switcher_browse = NULL;
  /* The two action rows are held with a reference of their own, see
     build_installed() and build_browse(); giving it back is what lets
     them go when the page does. */
  g_clear_object (&self->installed_actions);
  g_clear_object (&self->browse_actions);
  self->installed_group = NULL;
  self->installed_scroller = NULL;
  self->installed_status = NULL;
  self->installed_search_bar = NULL;
  self->installed_search_entry = NULL;
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
  self->pick_button = NULL;
  self->pick_badge = NULL;

  /* A batch that was still being installed is not applied any more: the
     plans and fetches hold the page weakly and stop. */
  g_clear_pointer (&self->batch, g_ptr_array_unref);
  self->picking = FALSE;
  self->pick_index = 0;

  if (self->debounce_source != 0)
    {
      g_source_remove (self->debounce_source);
      self->debounce_source = 0;
    }

  g_cancellable_cancel (self->browse);
  g_cancellable_cancel (self->scan);
  g_cancellable_cancel (self->more);
  g_clear_object (&self->browse);
  g_clear_object (&self->scan);
  g_clear_object (&self->more);
  g_clear_pointer (&self->seen_ids, g_hash_table_unref);

  /* The page of a project and the widgets in it are owned by the
     page; the fields only watch them, so they are dropped and not
     unrefed. */
  watch_widget (&self->details_body, NULL);
  watch_widget (&self->details_check, NULL);
  watch_widget (&self->details_dropdown, NULL);
  watch_widget (&self->details_download, NULL);
  watch_widget (&self->details_page, NULL);

  /* A batch of updates that was still being applied is not applied any
     more: the plans and fetches hold the page weakly and stop. */
  g_clear_pointer (&self->updates, g_ptr_array_unref);
  self->update_index = 0;
  self->updating = FALSE;

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
  g_clear_pointer (&self->project_icons, g_hash_table_unref);
  g_clear_pointer (&self->project_descriptions, g_hash_table_unref);
  g_clear_pointer (&self->project_titles, g_hash_table_unref);
  g_clear_pointer (&self->categories, g_ptr_array_unref);
  g_clear_pointer (&self->results, g_ptr_array_unref);
  g_list_free (self->installed_rows);
  g_list_free (self->result_rows);
  g_clear_pointer (&self->picks, g_ptr_array_unref);
  g_clear_pointer (&self->batch, g_ptr_array_unref);
  g_free (self->minecraft_version);
  g_free (self->loader);
  g_free (self->folder);
  g_free (self->query);
  g_free (self->current_project_title);
  g_free (self->current_project_id);
  g_free (self->details_author);
  g_clear_pointer (&self->details, aml_modrinth_details_free);
  g_clear_pointer (&self->details_versions, g_ptr_array_unref);
  g_free (self->details_problem);
  g_free (self->versions_problem);

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
  /* The list of what is picked is the page's own, and an instance that
     needs a mod loader to offer mods still has one: what is built here
     has to be there whatever the page goes on to show. Everything else
     is built in aml_content_page_new(), which knows the instance and the
     kind of content. */
  self->picks = g_ptr_array_new_with_free_func ((GDestroyNotify) pick_free);
}
