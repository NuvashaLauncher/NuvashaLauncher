/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-index.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-task.h"

/* Internal install/download workflow for NuvashaLauncherContentPage. */

/* ---------------------------------------------------------- *
 * Writing files
 * ---------------------------------------------------------- */

/* One file in an install plan. */
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
  gboolean required; /* required dependency, not the requested project */
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
install_file_of (NuvashaLauncherModrinthVersion *version, const char *replaces,
                 gboolean disabled, const char *project_name)
{
  NuvashaLauncherModrinthFile *file = nuvashalauncher_modrinth_version_primary_file (version);
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
  install->project_name = g_strdup (project_name);
  install->version_id = g_strdup (version->version_id);
  install->version_name = g_strdup (version->name);
  install->required_dependencies = g_ptr_array_new_with_free_func (g_free);
  install->optional_dependencies = g_ptr_array_new_with_free_func (g_free);
  install->incompatible_dependencies = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; version->dependencies != NULL &&
                   i < version->dependencies->len; i++)
    {
      NuvashaLauncherModrinthDependency *dependency =
        g_ptr_array_index (version->dependencies, i);
      GPtrArray *target = dependency->type == NUVASHALAUNCHER_DEPENDENCY_REQUIRED
                            ? install->required_dependencies
                            : dependency->type == NUVASHALAUNCHER_DEPENDENCY_OPTIONAL
                                ? install->optional_dependencies
                                : dependency->type == NUVASHALAUNCHER_DEPENDENCY_INCOMPATIBLE
                                    ? install->incompatible_dependencies : NULL;
      if (target != NULL && dependency->project_id != NULL)
        g_ptr_array_add (target, g_strdup (dependency->project_id));
    }
  install->label = g_strdup (version->name);

  name = disabled ? nuvashalauncher_content_disabled_name (file->filename, FALSE)
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

 NuvashaLauncherContentFile *
installed_project_file (NuvashaLauncherContentPage *self, const char *project_id)
{
  if (self == NULL || project_id == NULL || self->installed.files == NULL)
    return NULL;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      NuvashaLauncherContentFile *file = g_ptr_array_index (self->installed.files, i);

      if (g_strcmp0 (file->project_id, project_id) == 0)
        return file;
    }

  return NULL;
}

void
remove_duplicate_project_files (NuvashaLauncherContentPage *self, const char *project_id,
                                const char *keep_path)
{
  if (self == NULL || project_id == NULL || self->installed.files == NULL)
    return;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      NuvashaLauncherContentFile *installed = g_ptr_array_index (self->installed.files, i);
      g_autoptr (GError) error = NULL;

      if (g_strcmp0 (installed->project_id, project_id) != 0 ||
          g_strcmp0 (installed->path, keep_path) == 0)
        continue;

       if (!nuvashalauncher_content_remove_file (installed->path, &error) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
         {
           g_autofree char *message = g_strdup_printf (
             _("The duplicate file “%s” could not be removed: %s"),
             installed->path, nuvashalauncher_error_message (error));
           show_error (self, message, error);
         }
      else
        {
          g_autofree char *directory = content_page_base_directory (self);
          g_autofree char *filename = g_path_get_basename (installed->path);
          nuvashalauncher_task_scope_release_when_done (
            nuvashalauncher_content_index_remove_async (directory, self->type, filename,
                                            NULL, NULL), NULL);
        }
    }
}

/* A queue of files written one after the other, so that a big file never
 * sits in front of the one that needs it and a failure stops where it
 * happened. The page is only weakly held: it can go away while the last
 * file of it is still on its way, and the folder is kept here so the
 * download finishes without it. */
typedef struct {
  NuvashaLauncherContentPage *page;   /* weak: the page can go during the run */
  char *folder;           /* where the files go, kept for the whole run */
  GPtrArray *files;       /* InstallFile* */
  guint index;
  NuvashaLauncherDownload *download;  /* the manager's entry, held while the run lives */
  ContentInstallFinishedFunc on_finished;
  gpointer on_finished_data;
  GDestroyNotify on_finished_destroy;
} FetchRun;

static void fetch_next (FetchRun *run);

static void
fetch_notify (FetchRun *run, gboolean success)
{
  if (run->on_finished != NULL)
    {
      ContentInstallFinishedFunc cb = run->on_finished;
      gpointer data = run->on_finished_data;
      run->on_finished = NULL;
      cb (success, data);
    }
}

static void
fetch_free (FetchRun *run)
{
  fetch_notify (run, FALSE);

  if (run->on_finished_destroy != NULL)
    {
      GDestroyNotify destroy = run->on_finished_destroy;
      gpointer data = run->on_finished_data;
      run->on_finished_destroy = NULL;
      run->on_finished_data = NULL;
      destroy (data);
    }

  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page), (gpointer *) &run->page);

  if (run->download != NULL)
    nuvashalauncher_download_finish (run->download);

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
  gboolean ok = nuvashalauncher_download_file_finish (result, &error);
  InstallFile *file = NULL;

  if (run->index > 0)
    file = g_ptr_array_index (run->files, run->index - 1);

  /* A download that fails or is cancelled wrote nothing: a file only
   * reaches the folder complete and verified. */
  if (!ok)
    {
      gboolean cancelled = g_error_matches (error, NUVASHALAUNCHER_DOWNLOAD_ERROR,
                                            NUVASHALAUNCHER_DOWNLOAD_ERROR_CANCELLED);

      if (run->page != NULL)
        {
          NuvashaLauncherContentPage *page = run->page;

          /* A batch stops at the first file that fails rather than going
             on to the next with half of it missing. */
          if (page->install_model->updating)
            {
              g_clear_pointer (&page->install_model->updates, g_ptr_array_unref);
              page->install_model->update_index = 0;
              page->install_model->updating = FALSE;
            }

          /* A cancel is not a failure worth a message. */
          if (!cancelled)
             show_error (page, _("The file could not be installed"), error);
        }

      g_clear_error (&error);
      fetch_free (run);
      return;
    }

  /* Update the header progress after each file completes. */
  if (run->download != NULL)
    nuvashalauncher_download_set_progress (run->download, run->index, run->files->len);

  /* The new version is in place, so the file it replaces can go. */
  if (file != NULL && file->replaces != NULL)
    {
       if (!nuvashalauncher_content_remove_file (file->replaces, NULL) && run->page != NULL)
         {
           g_autofree char *message = g_strdup_printf (
             _("The replaced file “%s” could not be removed"),
             file->replaces);
           show_toast (run->page, message);
         }
    }

  if (run->page != NULL && file != NULL)
    {
      g_autofree char *keep = g_build_filename (run->folder, file->filename,
                                                 NULL);

      remove_duplicate_project_files (run->page, file->project_id, keep);

      {
        g_autofree char *instance_directory = content_page_base_directory (run->page);
        NuvashaLauncherContentIndexEntry entry = { 0 };
        entry.filename = file->filename;
         entry.hash = file->sha1;
         entry.hash512 = file->sha512;
         entry.download_url = file->url;
         entry.env_client = "required";
         entry.env_server = run->page->type == NUVASHALAUNCHER_CONTENT_MOD
                              ? "required" : "unsupported";
        entry.provider = "Modrinth";
        entry.project_id = file->project_id;
        entry.project_name = file->project_name;
        entry.icon_url = file->project_id != NULL && run->page->browse.model != NULL
                           ? g_hash_table_lookup (run->page->browse.model->project_icons,
                                                  file->project_id)
                           : NULL;
        entry.version_id = file->version_id;
        entry.version_name = file->version_name;
        entry.required_dependencies = file->required_dependencies;
        entry.optional_dependencies = file->optional_dependencies;
         entry.incompatible_dependencies = file->incompatible_dependencies;
         entry.env_client = "required";
         entry.env_server = run->page->type == NUVASHALAUNCHER_CONTENT_MOD
                              ? "required" : "unsupported";
        nuvashalauncher_task_scope_release_when_done (
          nuvashalauncher_content_index_update_async (instance_directory,
                                          run->page->type, &entry, NULL, NULL),
          nuvashalauncher_download_get_cancellable (run->download));
      }
    }

  fetch_next (run);
}

static void
fetch_next (FetchRun *run)
{
  NuvashaLauncherContentPage *self = run->page;
  InstallFile *file = NULL;
  g_autofree char *path = NULL;

  if (run->index >= run->files->len)
    {
      fetch_notify (run, TRUE);

      if (self != NULL)
        {
          if (self->install_model->updating)
            /* One update of the batch is in: start the next one. The
               list is read once, when the whole batch is done. */
            update_batch_next (self);
          else
            reload_installed (self);
        }

      fetch_free (run);
      return;
    }

  file = g_ptr_array_index (run->files, run->index);
  {
    g_autoptr (GError) error = NULL;

    if (!nuvashalauncher_content_ensure_folder (run->folder, &error))
      {
        if (self != NULL)
          show_error (self, _("The folder cannot be created"), error);

        fetch_free (run);
        return;
      }
  }

  path = g_build_filename (run->folder, file->filename, NULL);
  run->index++;

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), file->url, path, file->sha1, G_CHECKSUM_SHA1,
                             NULL, fetch_ready, run),
    nuvashalauncher_download_get_cancellable (run->download));
}

static NuvashaLauncherDownloadKind
content_download_kind (NuvashaLauncherContentType type)
{
  switch (type)
    {
    case NUVASHALAUNCHER_CONTENT_RESOURCE_PACK:
      return NUVASHALAUNCHER_DOWNLOAD_RESOURCE_PACK;

    case NUVASHALAUNCHER_CONTENT_SHADER_PACK:
      return NUVASHALAUNCHER_DOWNLOAD_SHADER;

    case NUVASHALAUNCHER_CONTENT_DATAPACK:
      return NUVASHALAUNCHER_DOWNLOAD_DATAPACK;

    case NUVASHALAUNCHER_CONTENT_MOD:
    default:
      return NUVASHALAUNCHER_DOWNLOAD_MOD;
    }
}

static void
start_fetch_with_callback (NuvashaLauncherContentPage *self,
                           GPtrArray *files,
                           ContentInstallFinishedFunc on_finished,
                           gpointer user_data,
                           GDestroyNotify destroy_data)
{
  FetchRun *run = g_new0 (FetchRun, 1);
  guint count = files != NULL ? files->len : 0;

  run->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &run->page);
  run->files = files;
  run->folder = g_strdup (self->folder);
  run->on_finished = on_finished;
  run->on_finished_data = user_data;
  run->on_finished_destroy = destroy_data;

  {
    const char *title = NULL;

    if (files != NULL && files->len > 0)
      title = ((InstallFile *) g_ptr_array_index (files, 0))->label;

    if (title == NULL)
       title = _("Content");

    /* Keep our own reference: nuvashalauncher_download_finish() removes the manager's
       reference when the run ends, while fetch_free() still releases the
       reference held by the run below. */
    run->download = g_object_ref (
        nuvashalauncher_downloads_add (content_download_kind (self->type), title));
     nuvashalauncher_download_set_detail (run->download, _("Installing"));
    nuvashalauncher_download_set_progress (run->download, 0, count);
  }

  fetch_next (run);
}

/* Takes the files of an install and writes them in order. The run owns
 * @files from here. */
static void
start_fetch (NuvashaLauncherContentPage *self, GPtrArray *files)
{
  start_fetch_with_callback (self, files, NULL, NULL, NULL);
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
  NuvashaLauncherContentPage *page;        /* weakly held */
  NuvashaLauncherModrinthVersion *version; /* transfer full, the version asked for */
  char *title;
  char *replaces;             /* the file an update takes the place of */
  gboolean stays_disabled;    /* an update of a disabled file stays so */
  GPtrArray *queue;           /* NuvashaLauncherModrinthDependency* to resolve */
  GPtrArray *files;           /* InstallFile* collected, dependencies */
  GHashTable *seen;           /* project ids already in the plan */
  GCancellable *cancellable;

  /* The versions request that has to answer before the plan moves on. */
  NuvashaLauncherTaskScope *operation;

  gboolean skip_confirm;
  ContentInstallFinishedFunc on_finished;
  gpointer on_finished_data;
  GDestroyNotify on_finished_destroy;
} PlanRun;

static void plan_free (PlanRun *plan);
static void show_plan (NuvashaLauncherContentPage *self, PlanRun *plan);
static void plan_versions_ready (GObject *source, GAsyncResult *result,
                                gpointer user_data);

static void
plan_next (PlanRun *plan)
{
  NuvashaLauncherModrinthDependency *dependency = NULL;
  g_autofree char *project_id = NULL;
  NuvashaLauncherContentPage *self = plan->page;

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

  /* The dependency is taken out of the queue and its project id copied out:
     the call below needs the id, and the dependency itself is not wanted
     once the question has been asked. */
  dependency = g_ptr_array_steal_index (plan->queue, 0);
  project_id = g_strdup (dependency->project_id);
  nuvashalauncher_modrinth_dependency_free (dependency);

  plan->operation = nuvashalauncher_modrinth_versions_async (project_id, self->type,
                                                 self->minecraft_version,
                                                 self->loader,
                                                 plan_versions_ready, plan);
  nuvashalauncher_task_scope_follow_cancellable (plan->operation, plan->cancellable);
}

static void
plan_free (PlanRun *plan)
{
  if (plan == NULL)
    return;

  if (plan->on_finished != NULL)
    {
      ContentInstallFinishedFunc cb = plan->on_finished;
      gpointer data = plan->on_finished_data;
      plan->on_finished = NULL;
      cb (FALSE, data);
    }

  if (plan->on_finished_destroy != NULL)
    {
      GDestroyNotify destroy = plan->on_finished_destroy;
      gpointer data = plan->on_finished_data;
      plan->on_finished_destroy = NULL;
      plan->on_finished_data = NULL;
      destroy (data);
    }

  if (plan->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (plan->page),
                                  (gpointer *) &plan->page);

  g_clear_pointer (&plan->version, nuvashalauncher_modrinth_version_free);
  g_clear_pointer (&plan->queue, g_ptr_array_unref);
  g_clear_pointer (&plan->files, g_ptr_array_unref);
  g_clear_pointer (&plan->seen, g_hash_table_unref);
  g_clear_object (&plan->cancellable);

  if (plan->operation != NULL)
    nuvashalauncher_task_scope_unref (plan->operation);

  g_free (plan->title);
  g_free (plan->replaces);
  g_free (plan);
}

/* Build confirmation text for dependencies and conflicts, or NULL when
 * the install needs no additional explanation. */
static char *
plan_text (NuvashaLauncherContentPage *self, PlanRun *plan)
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
                               ? g_hash_table_lookup (self->browse.model->project_titles,
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
          NuvashaLauncherModrinthDependency *dependency =
            g_ptr_array_index (plan->version->dependencies, i);

          if (dependency->type != NUVASHALAUNCHER_DEPENDENCY_INCOMPATIBLE)
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

           g_string_append (text, _("Conflicts\n"));
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
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  PlanRun *plan = g_object_get_data (G_OBJECT (source), "nuvashalauncher-plan");
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
show_plan (NuvashaLauncherContentPage *self, PlanRun *plan)
{
  /* An update of the whole list was confirmed once, as a whole, and direct
     version-picker downloads start immediately without a confirmation dialog. */
  if (self->install_model->updating || plan->skip_confirm)
    {
      ContentInstallFinishedFunc cb = plan->on_finished;
      gpointer data = plan->on_finished_data;
      GDestroyNotify destroy = plan->on_finished_destroy;

      plan->on_finished = NULL;
      plan->on_finished_data = NULL;
      plan->on_finished_destroy = NULL;

      start_fetch_with_callback (self, plan->files, cb, data, destroy);
      plan->files = NULL; /* the fetch owns them now */
      plan_free (plan);
      return;
    }

  {
    const char *action = _("Install");
    /* The name Modrinth writes already holds the version: "Xaero's
       Minimap 26.6.0". */
    const char *title = plan->title != NULL ? plan->title : "the file";
       g_autofree char *heading =
       g_strdup_printf (_("%s “%s”"),
                         action == NULL ? _("Install") : action, title);
     g_autofree char *body = plan_text (self, plan);
    AdwAlertDialog *alert =
      ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body));

    adw_alert_dialog_set_heading_use_markup (alert, FALSE);
    adw_alert_dialog_set_body_use_markup (alert, FALSE);
     adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
     adw_alert_dialog_add_response (alert, "install",
                                   action == NULL ? _("Install") : action);
    adw_alert_dialog_set_response_appearance (alert, "install",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (alert, "cancel");
    adw_alert_dialog_set_close_response (alert, "cancel");

    g_object_set_data (G_OBJECT (alert), "nuvashalauncher-plan", plan);
    adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_plan_chosen,
                             g_object_ref (self));
  }
}

/* Dependencies required by @version that are missing from the plan.
 * that names nothing to fetch stops the plan instead of writing half of
 * it. */
static gboolean
plan_take (PlanRun *plan, NuvashaLauncherModrinthVersion *version)
{
  NuvashaLauncherContentPage *self = plan->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) pending = NULL;

  pending = nuvashalauncher_modrinth_resolve_install (version, NULL, NULL, &error);

  if (pending == NULL)
    {
      if (self != NULL)
        {
           show_error (self, _("The file could not be installed"), error);
          plan_stopped (self);
        }

      return FALSE;
    }

  for (guint i = 0; i < pending->len; i++)
    {
      NuvashaLauncherModrinthDependency *dependency = g_ptr_array_index (pending, i);

      /* One copy of a project is one file: the second time round it is
       * already in the plan. */
      if (g_hash_table_contains (plan->seen, dependency->project_id))
        continue;

      g_hash_table_add (plan->seen, g_strdup (dependency->project_id));
      g_ptr_array_add (plan->queue,
                       nuvashalauncher_modrinth_dependency_copy (dependency));
    }

  return TRUE;
}

static void
plan_versions_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  PlanRun *plan = user_data;
  NuvashaLauncherContentPage *self = plan->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  NuvashaLauncherModrinthVersion *version = NULL;
  InstallFile *file = NULL;

  versions = nuvashalauncher_modrinth_versions_finish (result, &error);

  /* This answer is in, so the plan does not need the handle that brought
     it; what is left of the plan owns its own work from here. */
  if (plan->operation != NULL)
    {
      nuvashalauncher_task_scope_unref (plan->operation);
      plan->operation = NULL;
    }

  if (versions == NULL || versions->len == 0)
    {
      /* What a project publishes for nothing that fits is said in the
       * words the page shows everywhere else. */
      if (self != NULL)
        {
          show_error (self,
                      versions == NULL ? _("Could not load versions. Try again.")
                                       : _("This project publishes nothing that fits this instance."),
                      error);
          plan_stopped (self);
        }

      plan_free (plan);
      return;
    }

  /* The newest compatible version is first. */
  version = g_ptr_array_steal_index (versions, 0);
  {
    const char *project_name = NULL;
    NuvashaLauncherContentFile *installed = installed_project_file (self,
                                                         version->project_id);

    if (self != NULL && self->browse.model != NULL)
      project_name = g_hash_table_lookup (self->browse.model->project_titles,
                                          version->project_id);

    file = install_file_of (version, installed != NULL ? installed->path : NULL,
                            installed != NULL && !installed->enabled,
                            project_name);
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
      nuvashalauncher_modrinth_version_free (version);
      plan_free (plan);
      return;
    }

  nuvashalauncher_modrinth_version_free (version);
  plan_next (plan);
}

static void
start_plan_internal (NuvashaLauncherContentPage *self,
                     NuvashaLauncherModrinthVersion *version,
                     const char *title,
                     const char *replaces,
                     gboolean disabled,
                     gboolean skip_confirm,
                     ContentInstallFinishedFunc on_finished,
                     gpointer user_data,
                     GDestroyNotify destroy_data)
{
  PlanRun *plan = g_new0 (PlanRun, 1);

  plan->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &plan->page);
  plan->version = version; /* the plan owns it from here */
  plan->title = g_strdup (title);
  if (replaces == NULL)
    {
      NuvashaLauncherContentFile *installed = installed_project_file (self,
                                                           version->project_id);

      replaces = installed != NULL ? installed->path : NULL;
      disabled = installed != NULL && !installed->enabled;
    }
  plan->replaces = replaces != NULL ? g_strdup (replaces) : NULL;
  plan->stays_disabled = disabled;
  plan->skip_confirm = skip_confirm;
  plan->on_finished = on_finished;
  plan->on_finished_data = user_data;
  plan->on_finished_destroy = destroy_data;
  plan->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) install_file_free);
  plan->queue = g_ptr_array_new_with_free_func (
      (GDestroyNotify) nuvashalauncher_modrinth_dependency_free);
  plan->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  plan->cancellable = g_object_ref (self->browse.model->cancellable);

  if (!plan_take (plan, version))
    {
      plan_free (plan);
      return;
    }

  /* Show the plan first: the file itself is listed in it, and only the
   * fetch writes anything to the folder. */
  plan_next (plan);
}

/* The install of one version and of everything it requires. */
void
start_plan (NuvashaLauncherContentPage *self, NuvashaLauncherModrinthVersion *version,
            const char *title, const char *replaces, gboolean disabled)
{
  start_plan_internal (self, version, title, replaces, disabled,
                       FALSE, NULL, NULL, NULL);
}

void
start_plan_direct (NuvashaLauncherContentPage *self,
                   NuvashaLauncherModrinthVersion *version,
                   const char *title,
                   ContentInstallFinishedFunc on_finished,
                   gpointer user_data,
                   GDestroyNotify destroy_data)
{
  start_plan_internal (self, version, title, NULL, FALSE,
                       TRUE, on_finished, user_data, destroy_data);
}

/* ---------------------------------------------------------- *
 * The batch install plan for multiple selected items
 * ---------------------------------------------------------- */

typedef struct {
  NuvashaLauncherContentPage *page;
  GPtrArray *pending_projects;  /* NuvashaLauncherModrinthProject* */
  GPtrArray *selected_titles;   /* char* */
  GPtrArray *files;             /* InstallFile* (all files to install) */
  GPtrArray *queue;             /* NuvashaLauncherModrinthDependency* */
  GPtrArray *dependency_titles; /* char* */
  GHashTable *seen;             /* project_id -> TRUE */
  GCancellable *cancellable;
  NuvashaLauncherTaskScope *operation;
} BatchPlanRun;

static void batch_plan_resolve_next_project (BatchPlanRun *batch);
static void batch_plan_resolve_next_dependency (BatchPlanRun *batch);
static void show_batch_plan (NuvashaLauncherContentPage *self, BatchPlanRun *batch);

static void
batch_plan_free (BatchPlanRun *batch)
{
  if (batch == NULL)
    return;
  if (batch->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (batch->page), (gpointer *) &batch->page);
  g_clear_pointer (&batch->pending_projects, g_ptr_array_unref);
  g_clear_pointer (&batch->selected_titles, g_ptr_array_unref);
  g_clear_pointer (&batch->files, g_ptr_array_unref);
  g_clear_pointer (&batch->queue, g_ptr_array_unref);
  g_clear_pointer (&batch->dependency_titles, g_ptr_array_unref);
  g_clear_pointer (&batch->seen, g_hash_table_unref);
  g_clear_object (&batch->cancellable);
  if (batch->operation != NULL)
    nuvashalauncher_task_scope_unref (batch->operation);
  g_free (batch);
}

static void
on_batch_project_versions_ready (GObject *source G_GNUC_UNUSED,
                                 GAsyncResult *result,
                                 gpointer user_data)
{
  BatchPlanRun *batch = user_data;
  NuvashaLauncherContentPage *self = batch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions =
    nuvashalauncher_modrinth_versions_finish (result, &error);

  if (self == NULL || (batch->cancellable != NULL && g_cancellable_is_cancelled (batch->cancellable)))
    {
      batch_plan_free (batch);
      return;
    }

  if (versions != NULL && versions->len > 0)
    {
      NuvashaLauncherModrinthVersion *version = g_ptr_array_index (versions, 0);
      NuvashaLauncherModrinthProject *current = g_ptr_array_index (batch->pending_projects, 0);
      const char *title = current->title != NULL ? current->title : version->name;

      g_ptr_array_add (batch->selected_titles, g_strdup (title));
      g_hash_table_add (batch->seen, g_strdup (version->project_id));

      InstallFile *own = install_file_of (version, NULL, FALSE, current->title);
      if (own != NULL)
        g_ptr_array_add (batch->files, own);

      g_autoptr (GPtrArray) pending =
        nuvashalauncher_modrinth_resolve_install (version, NULL, NULL, NULL);
      if (pending != NULL)
        {
          for (guint i = 0; i < pending->len; i++)
            {
              NuvashaLauncherModrinthDependency *dep = g_ptr_array_index (pending, i);
              if (!g_hash_table_contains (batch->seen, dep->project_id))
                {
                  g_hash_table_add (batch->seen, g_strdup (dep->project_id));
                  g_ptr_array_add (batch->queue, nuvashalauncher_modrinth_dependency_copy (dep));
                }
            }
        }
    }

  g_ptr_array_remove_index (batch->pending_projects, 0);
  batch_plan_resolve_next_project (batch);
}

static void
batch_plan_resolve_next_project (BatchPlanRun *batch)
{
  NuvashaLauncherContentPage *self = batch->page;
  if (self == NULL)
    {
      batch_plan_free (batch);
      return;
    }

  if (batch->pending_projects->len > 0)
    {
      NuvashaLauncherModrinthProject *p = g_ptr_array_index (batch->pending_projects, 0);
      batch->operation = nuvashalauncher_modrinth_versions_async (
        p->project_id, self->type, self->minecraft_version, self->loader,
        on_batch_project_versions_ready, batch);
      if (batch->cancellable != NULL)
        nuvashalauncher_task_scope_follow_cancellable (batch->operation, batch->cancellable);
      return;
    }

  batch_plan_resolve_next_dependency (batch);
}

static void
on_batch_dependency_versions_ready (GObject *source G_GNUC_UNUSED,
                                    GAsyncResult *result,
                                    gpointer user_data)
{
  BatchPlanRun *batch = user_data;
  NuvashaLauncherContentPage *self = batch->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions =
    nuvashalauncher_modrinth_versions_finish (result, &error);

  if (self == NULL || (batch->cancellable != NULL && g_cancellable_is_cancelled (batch->cancellable)))
    {
      batch_plan_free (batch);
      return;
    }

  if (versions != NULL && versions->len > 0)
    {
      NuvashaLauncherModrinthVersion *version = g_ptr_array_index (versions, 0);
      const char *title = version->name != NULL ? version->name : version->project_id;
      g_ptr_array_add (batch->dependency_titles, g_strdup (title));

      InstallFile *file = install_file_of (version, NULL, FALSE, NULL);
      if (file != NULL)
        {
          file->required = TRUE;
          g_ptr_array_add (batch->files, file);
        }

      g_autoptr (GPtrArray) pending =
        nuvashalauncher_modrinth_resolve_install (version, NULL, NULL, NULL);
      if (pending != NULL)
        {
          for (guint i = 0; i < pending->len; i++)
            {
              NuvashaLauncherModrinthDependency *dep = g_ptr_array_index (pending, i);
              if (!g_hash_table_contains (batch->seen, dep->project_id))
                {
                  g_hash_table_add (batch->seen, g_strdup (dep->project_id));
                  g_ptr_array_add (batch->queue, nuvashalauncher_modrinth_dependency_copy (dep));
                }
            }
        }
    }

  batch_plan_resolve_next_dependency (batch);
}

static void
batch_plan_resolve_next_dependency (BatchPlanRun *batch)
{
  NuvashaLauncherContentPage *self = batch->page;
  if (self == NULL)
    {
      batch_plan_free (batch);
      return;
    }

  if (batch->queue->len > 0)
    {
      NuvashaLauncherModrinthDependency *dep = g_ptr_array_steal_index (batch->queue, 0);
      g_autofree char *project_id = g_strdup (dep->project_id);
      nuvashalauncher_modrinth_dependency_free (dep);

      batch->operation = nuvashalauncher_modrinth_versions_async (
        project_id, self->type, self->minecraft_version, self->loader,
        on_batch_dependency_versions_ready, batch);
      if (batch->cancellable != NULL)
        nuvashalauncher_task_scope_follow_cancellable (batch->operation, batch->cancellable);
      return;
    }

  show_batch_plan (self, batch);
}

static void
on_batch_plan_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  BatchPlanRun *batch = g_object_get_data (G_OBJECT (source), "nuvashalauncher-batch-plan");
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (batch == NULL)
    {
      g_object_unref (self);
      return;
    }

  if (g_strcmp0 (response, "install") != 0 || batch->page == NULL)
    {
      batch_plan_free (batch);
      g_object_unref (self);
      return;
    }

  guint count = batch->selected_titles->len;
  const char *msg_format = NULL;
  switch (self->type)
    {
    case NUVASHALAUNCHER_CONTENT_MOD:
      msg_format = ngettext ("%u mod is being installed", "%u mods are being installed", count);
      break;
    case NUVASHALAUNCHER_CONTENT_RESOURCE_PACK:
      msg_format = ngettext ("%u resource pack is being installed", "%u resource packs are being installed", count);
      break;
    case NUVASHALAUNCHER_CONTENT_SHADER_PACK:
      msg_format = ngettext ("%u shader is being installed", "%u shaders are being installed", count);
      break;
    case NUVASHALAUNCHER_CONTENT_DATAPACK:
      msg_format = ngettext ("%u datapack is being installed", "%u datapacks are being installed", count);
      break;
    default:
      msg_format = ngettext ("%u item is being installed", "%u items are being installed", count);
      break;
    }
  g_autofree char *toast_msg = g_strdup_printf (msg_format, count);
  show_toast (self, toast_msg);

  start_fetch (self, batch->files);
  batch->files = NULL;
  batch_plan_free (batch);
  g_object_unref (self);
}

static void
show_batch_plan (NuvashaLauncherContentPage *self, BatchPlanRun *batch)
{
  if (batch->files == NULL || batch->files->len == 0)
    {
      batch_plan_free (batch);
      return;
    }

  guint count = batch->selected_titles->len;
  const char *heading_format = NULL;
  switch (self->type)
    {
    case NUVASHALAUNCHER_CONTENT_MOD:
      heading_format = ngettext ("Install the following mod?", "Install the following mods?", count);
      break;
    case NUVASHALAUNCHER_CONTENT_RESOURCE_PACK:
      heading_format = ngettext ("Install the following resource pack?", "Install the following resource packs?", count);
      break;
    case NUVASHALAUNCHER_CONTENT_SHADER_PACK:
      heading_format = ngettext ("Install the following shader?", "Install the following shaders?", count);
      break;
    case NUVASHALAUNCHER_CONTENT_DATAPACK:
      heading_format = ngettext ("Install the following datapack?", "Install the following datapacks?", count);
      break;
    default:
      heading_format = ngettext ("Install the following item?", "Install the following items?", count);
      break;
    }

  g_autofree char *heading = g_strdup (heading_format);

  g_autoptr (GString) body = g_string_new (NULL);
  for (guint i = 0; i < batch->selected_titles->len; i++)
    {
      const char *title = g_ptr_array_index (batch->selected_titles, i);
      g_string_append_printf (body, "%s\n", title);
    }

  if (batch->dependency_titles->len > 0)
    {
      g_string_append_printf (body, "\n%s\n", _("Dependencies:"));
      for (guint i = 0; i < batch->dependency_titles->len; i++)
        {
          const char *title = g_ptr_array_index (batch->dependency_titles, i);
          g_string_append_printf (body, "%s\n", title);
        }
    }

  while (body->len > 0 && body->str[body->len - 1] == '\n')
    g_string_truncate (body, body->len - 1);

  AdwAlertDialog *alert =
    ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, body->str));

  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (alert, "install", _("Install"));
  adw_alert_dialog_set_response_appearance (alert, "install",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  g_object_set_data (G_OBJECT (alert), "nuvashalauncher-batch-plan", batch);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_batch_plan_chosen,
                           g_object_ref (self));
}

void
nuvashalauncher_content_page_start_batch_plan (NuvashaLauncherContentPage *self,
                                               GPtrArray *projects)
{
  if (self == NULL || projects == NULL || projects->len == 0)
    {
      if (projects != NULL)
        g_ptr_array_unref (projects);
      return;
    }

  BatchPlanRun *batch = g_new0 (BatchPlanRun, 1);
  batch->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &batch->page);
  batch->pending_projects = projects;
  batch->selected_titles = g_ptr_array_new_with_free_func (g_free);
  batch->files = g_ptr_array_new_with_free_func ((GDestroyNotify) install_file_free);
  batch->queue = g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_modrinth_dependency_free);
  batch->dependency_titles = g_ptr_array_new_with_free_func (g_free);
  batch->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (self->browse.model != NULL && self->browse.model->cancellable != NULL)
    batch->cancellable = g_object_ref (self->browse.model->cancellable);

  batch_plan_resolve_next_project (batch);
}
