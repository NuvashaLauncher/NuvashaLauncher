/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-page-private.h"
#include "aml-content-index.h"
#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-i18n.h"
#include "aml-modrinth.h"

/* Internal install/download workflow for AmlContentPage. */

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

 AmlContentFile *
installed_project_file (AmlContentPage *self, const char *project_id)
{
  if (self == NULL || project_id == NULL || self->installed.files == NULL)
    return NULL;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      AmlContentFile *file = g_ptr_array_index (self->installed.files, i);

      if (g_strcmp0 (file->project_id, project_id) == 0)
        return file;
    }

  return NULL;
}

void
remove_duplicate_project_files (AmlContentPage *self, const char *project_id,
                                const char *keep_path)
{
  if (self == NULL || project_id == NULL)
    return;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      AmlContentFile *installed = g_ptr_array_index (self->installed.files, i);
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
          if (page->install.updating)
            {
              g_clear_pointer (&page->install.updates, g_ptr_array_unref);
              page->install.update_index = 0;
              page->install.updating = FALSE;
            }

          if (page->install.picking)
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
          if (self->install.updating)
            /* One update of the batch is in: start the next one. The
               list is read once, when the whole batch is done. */
            update_batch_next (self);
          else if (self->install.picking)
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
      title = self->project.current_project_title;

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
                               ? g_hash_table_lookup (self->browse.project_titles,
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
      g_strdup_printf (_("%s is being installed"), plan->title);

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
  if (self->install.updating || self->install.picking)
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
                                          : _("no compatible version"));
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
                            g_hash_table_lookup (self->browse.project_titles,
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
void
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
  plan->cancellable = g_object_ref (self->browse.cancellable);

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
struct _Pick {
  char *project_id;
  char *title;
  AmlModrinthVersion *version; /* owned */
};

void
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
  if (self->install.picks == NULL || project_id == NULL)
    return NULL;

  for (guint i = 0; i < self->install.picks->len; i++)
    {
      Pick *pick = g_ptr_array_index (self->install.picks, i);

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
void
refresh_pick_check (AmlContentPage *self)
{
  AmlModrinthVersion *version = NULL;
  gboolean on;

  if (self->project.details_check == NULL || self->project.details_dropdown == NULL)
    return;

  version = selected_version (self->project.details_dropdown,
                              self->project.details_versions);
  on = pick_is_version (pick_of_project (
                          self, version != NULL ? version->project_id : NULL),
                        version);

  g_object_set_data (G_OBJECT (self->project.details_check), "aml-setting",
                     GINT_TO_POINTER (1));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->project.details_check), on);
  g_object_set_data (G_OBJECT (self->project.details_check), "aml-setting", NULL);
}

/* What the page has picked is said to the header, which counts it on the
   icon of its downloads button, and put on the button at the foot of the
   list. Called after every change of the list, and once when the list is
   built, so that the button starts where the picks start. */
void
aml_content_page_refresh_picks (AmlContentPage *self)
{
  guint count = self->install.picks != NULL ? self->install.picks->len : 0;
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
void
pick_add (AmlContentPage *self, AmlModrinthVersion *version)
{
  Pick *pick = NULL;

  if (self->install.picks == NULL || version == NULL || version->project_id == NULL)
    return;

  pick = pick_of_project (self, version->project_id);

  if (pick != NULL)
    {
      /* The very version that was already on the list is left alone, so
         the checkmarks do not move under the click that put it there. */
      if (pick_is_version (pick, version))
        return;

      g_ptr_array_remove (self->install.picks, pick);
    }

  pick = g_new0 (Pick, 1);
  pick->project_id = g_strdup (version->project_id);
  pick->title = g_strdup (self->project.current_project_title != NULL
                            ? self->project.current_project_title
                            : version_name_of (version));
  pick->version = aml_modrinth_version_copy (version);
  g_ptr_array_add (self->install.picks, pick);
}

/* The project of @version has nothing picked any more. Only its own
   entry goes: another version of the same project is a different pick. */
void
pick_remove (AmlContentPage *self, AmlModrinthVersion *version)
{
  Pick *pick = NULL;

  if (self->install.picks == NULL || version == NULL)
    return;

  pick = pick_of_project (self, version->project_id);

  if (pick_is_version (pick, version))
    g_ptr_array_remove (self->install.picks, pick);
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
      if (self->install.batch != NULL)
        {
          g_clear_pointer (&self->install.picks, g_ptr_array_unref);
          self->install.picks = g_steal_pointer (&self->install.batch);
          aml_content_page_refresh_picks (self);
        }

      g_object_unref (self);
      return;
    }

  if (self->install.batch == NULL)
    {
      g_object_unref (self);
      return;
    }

  self->install.picking = TRUE;
  self->install.pick_index = 0;
  batch_next (self);

  g_object_unref (self);
}

/* Starts the next version of the confirmed list, or says that the whole
   of it is in. Each one is planned and fetched like any other install;
   the confirmation is not asked again for the ones that follow. */
void
batch_next (AmlContentPage *self)
{
  Pick *pick = NULL;

  if (self->install.batch == NULL || self->install.pick_index >= self->install.batch->len)
    {
      guint done = self->install.batch != NULL ? self->install.batch->len : 0;

      g_clear_pointer (&self->install.batch, g_ptr_array_unref);
      self->install.picking = FALSE;
      self->install.pick_index = 0;
      set_busy (self, FALSE);

      if (done > 0)
        {
          g_autofree char *message = g_strdup_printf (
            ngettext ("Installed %u item", "Installed %u items", done), done);

          show_toast (self, message);
          reload_installed (self);
        }

      return;
    }

  pick = g_ptr_array_index (self->install.batch, self->install.pick_index);
  self->install.pick_index++;

  set_busy (self, TRUE);
  start_plan (self, aml_modrinth_version_copy (pick->version), pick->title,
              NULL, FALSE);
}

/* A file of the batch could not be installed: what it would take is not
   there, so the list goes no further. */
void
batch_stopped (AmlContentPage *self)
{
  g_clear_pointer (&self->install.batch, g_ptr_array_unref);
  self->install.picking = FALSE;
  self->install.pick_index = 0;
}

void
aml_content_page_clear_picks (AmlContentPage *self)
{
  g_clear_pointer (&self->install.picks, g_ptr_array_unref);
  self->install.picks = g_ptr_array_new_with_free_func ((GDestroyNotify) pick_free);
  aml_content_page_refresh_picks (self);
}

/* The button at the foot of the list: every picked version, one per
   line, and the choice to install the whole of it at once. */
void
aml_content_page_on_pick_button_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;
  g_autoptr (GString) body = NULL;
  g_autoptr (GHashTable) listed = NULL;
  AdwAlertDialog *alert = NULL;
  const char *kind = NULL;

  if (self->install.picks == NULL || self->install.picks->len == 0)
    return;

  body = g_string_new (NULL);
  listed = g_hash_table_new (g_str_hash, g_str_equal);

  kind = aml_content_type_title (self->type);

  for (guint i = 0; i < self->install.picks->len; i++)
    {
      Pick *pick = g_ptr_array_index (self->install.picks, i);

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

    for (guint i = 0; i < self->install.picks->len; i++)
      {
        Pick *pick = g_ptr_array_index (self->install.picks, i);

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
            title = g_hash_table_lookup (self->browse.project_titles,
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
  g_clear_pointer (&self->install.batch, g_ptr_array_unref);
  self->install.batch = g_steal_pointer (&self->install.picks);
  self->install.picks = g_ptr_array_new_with_free_func ((GDestroyNotify) pick_free);
  aml_content_page_refresh_picks (self);

  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_batch_chosen,
                           g_object_ref (self));
}
