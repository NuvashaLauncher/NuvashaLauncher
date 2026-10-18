/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-content-page-private.h"
#include "comfy-content-index.h"
#include "comfy-download.h"
#include "comfy-downloads.h"
#include "comfy-i18n.h"
#include "comfy-modrinth.h"
#include "comfy-operation.h"

/* Internal install/download workflow for ComfyContentPage. */

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
install_file_of (ComfyModrinthVersion *version, const char *replaces,
                 gboolean disabled, const char *project_name)
{
  ComfyModrinthFile *file = comfy_modrinth_version_primary_file (version);
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
      ComfyModrinthDependency *dependency =
        g_ptr_array_index (version->dependencies, i);
      GPtrArray *target = dependency->type == comfy_DEPENDENCY_REQUIRED
                            ? install->required_dependencies
                            : dependency->type == comfy_DEPENDENCY_OPTIONAL
                                ? install->optional_dependencies
                                : dependency->type == comfy_DEPENDENCY_INCOMPATIBLE
                                    ? install->incompatible_dependencies : NULL;
      if (target != NULL && dependency->project_id != NULL)
        g_ptr_array_add (target, g_strdup (dependency->project_id));
    }
  install->label = g_strdup (version->name);

  name = disabled ? comfy_content_disabled_name (file->filename, FALSE)
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

 ComfyContentFile *
installed_project_file (ComfyContentPage *self, const char *project_id)
{
  if (self == NULL || project_id == NULL || self->installed.files == NULL)
    return NULL;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      ComfyContentFile *file = g_ptr_array_index (self->installed.files, i);

      if (g_strcmp0 (file->project_id, project_id) == 0)
        return file;
    }

  return NULL;
}

void
remove_duplicate_project_files (ComfyContentPage *self, const char *project_id,
                                const char *keep_path)
{
  if (self == NULL || project_id == NULL || self->installed.files == NULL)
    return;

  for (guint i = 0; i < self->installed.files->len; i++)
    {
      ComfyContentFile *installed = g_ptr_array_index (self->installed.files, i);
      g_autoptr (GError) error = NULL;

      if (g_strcmp0 (installed->project_id, project_id) != 0 ||
          g_strcmp0 (installed->path, keep_path) == 0)
        continue;

       if (!comfy_content_remove_file (installed->path, &error) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_warning ("the duplicate file \"%s\" could not be removed: %s",
                   installed->path, error != NULL ? error->message : "unknown error");
      else
        {
          g_autofree char *directory = NULL;
          g_autofree char *filename = g_path_get_basename (installed->path);
          g_object_get (self->instance, "directory", &directory, NULL);
          comfy_operation_release (
            comfy_content_index_remove_async (directory, self->type, filename,
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
  ComfyContentPage *page;   /* weak: the page can go during the run */
  char *folder;           /* where the files go, kept for the whole run */
  GPtrArray *files;       /* InstallFile* */
  guint index;
  ComfyDownload *download;  /* the manager's entry, held while the run lives */
} FetchRun;

static void fetch_next (FetchRun *run);

static void
fetch_free (FetchRun *run)
{
  if (run->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (run->page), (gpointer *) &run->page);

  if (run->download != NULL)
    comfy_download_finish (run->download);

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
  gboolean ok = comfy_download_file_finish (result, &error);
  InstallFile *file = NULL;

  if (run->index > 0)
    file = g_ptr_array_index (run->files, run->index - 1);

  /* A download that fails or is cancelled wrote nothing: a file only
   * reaches the folder complete and verified. */
  if (!ok)
    {
      gboolean cancelled = g_error_matches (error, comfy_DOWNLOAD_ERROR,
                                            comfy_DOWNLOAD_ERROR_CANCELLED);

      if (run->page != NULL)
        {
          ComfyContentPage *page = run->page;

          /* A batch stops at the first file that fails rather than going
             on to the next with half of it missing. */
          if (page->install_model->updating)
            {
              g_clear_pointer (&page->install_model->updates, g_ptr_array_unref);
              page->install_model->update_index = 0;
              page->install_model->updating = FALSE;
            }

          if (page->install_model->picking)
            batch_stopped (page);

          set_busy (page, FALSE);

          /* A cancel is not a failure worth a message. */
          if (!cancelled)
             show_error (page, _("The file could not be installed"), error);
        }

      g_clear_error (&error);
      fetch_free (run);
      return;
    }

  /* What the header row shows: the phase line stays, the count on the
     right moves with each file landed. */
  if (run->download != NULL)
    comfy_download_set_progress (run->download, run->index, run->files->len);

  /* The new version is in place, so the file it replaces can go. */
  if (file != NULL && file->replaces != NULL)
    {
       if (!comfy_content_remove_file (file->replaces, NULL))
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
        ComfyContentIndexEntry entry = { 0 };
        g_object_get (run->page->instance, "directory", &instance_directory,
                      NULL);
        entry.filename = file->filename;
         entry.hash = file->sha1;
         entry.hash512 = file->sha512;
         entry.download_url = file->url;
         entry.env_client = "required";
         entry.env_server = run->page->type == comfy_CONTENT_MOD
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
         entry.env_server = run->page->type == comfy_CONTENT_MOD
                              ? "required" : "unsupported";
        comfy_operation_release (
          comfy_content_index_update_async (instance_directory,
                                          run->page->type, &entry, NULL, NULL),
          comfy_download_get_cancellable (run->download));
      }
    }

  fetch_next (run);
}

static void
fetch_next (FetchRun *run)
{
  ComfyContentPage *self = run->page;
  InstallFile *file = NULL;
  g_autofree char *path = NULL;

  if (run->index >= run->files->len)
    {
      if (self != NULL)
        {
          if (self->install_model->updating)
            /* One update of the batch is in: start the next one. The
               list is read once, when the whole batch is done. */
            update_batch_next (self);
          else if (self->install_model->picking)
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
  {
    g_autoptr (GError) error = NULL;

    if (!comfy_content_ensure_folder (run->folder, &error))
      {
        if (self != NULL)
          {
            set_busy (self, FALSE);
       show_error (self, _("The folder cannot be created"), error);
          }

        fetch_free (run);
        return;
      }
  }

  path = g_build_filename (run->folder, file->filename, NULL);
  run->index++;

  comfy_operation_release (
    comfy_download_file_async (comfy_downloads_get_service (), file->url, path, file->sha1, G_CHECKSUM_SHA1,
                             NULL, fetch_ready, run),
    comfy_download_get_cancellable (run->download));
}

static ComfyDownloadKind
content_download_kind (ComfyContentType type)
{
  switch (type)
    {
    case comfy_CONTENT_RESOURCE_PACK:
      return comfy_DOWNLOAD_RESOURCE_PACK;

    case comfy_CONTENT_SHADER_PACK:
      return comfy_DOWNLOAD_SHADER;

    case comfy_CONTENT_MOD:
    default:
      return comfy_DOWNLOAD_MOD;
    }
}

/* Takes the files of an install and writes them in order. The run owns
 * @files from here. */
static void
start_fetch (ComfyContentPage *self, GPtrArray *files)
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

    /* Keep our own reference: comfy_download_finish() removes the manager's
       reference when the run ends, while fetch_free() still releases the
       reference held by the run below. */
    run->download = g_object_ref (
        comfy_downloads_add (content_download_kind (self->type), title));
    comfy_download_set_detail (run->download, "Installing");
    comfy_download_set_progress (run->download, 0, count);
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
  ComfyContentPage *page;        /* weakly held */
  ComfyModrinthVersion *version; /* transfer full, the version asked for */
  char *title;
  char *replaces;             /* the file an update takes the place of */
  gboolean stays_disabled;    /* an update of a disabled file stays so */
  GPtrArray *queue;           /* ComfyModrinthDependency* to resolve */
  GPtrArray *files;           /* InstallFile* collected, dependencies */
  GHashTable *seen;           /* project ids already in the plan */
  GCancellable *cancellable;

  /* The versions request that has to answer before the plan moves on. */
  ComfyTaskScope *operation;
} PlanRun;

static void plan_free (PlanRun *plan);
static void show_plan (ComfyContentPage *self, PlanRun *plan);
static void plan_versions_ready (GObject *source, GAsyncResult *result,
                                gpointer user_data);

static void
plan_next (PlanRun *plan)
{
  ComfyModrinthDependency *dependency = NULL;
  g_autofree char *project_id = NULL;
  ComfyContentPage *self = plan->page;

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
  comfy_modrinth_dependency_free (dependency);

  plan->operation = comfy_modrinth_versions_async (project_id, self->type,
                                                 self->minecraft_version,
                                                 self->loader,
                                                 plan_versions_ready, plan);
  comfy_operation_follow (plan->operation, plan->cancellable);
}

static void
plan_free (PlanRun *plan)
{
  if (plan == NULL)
    return;

  if (plan->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (plan->page),
                                  (gpointer *) &plan->page);

  g_clear_pointer (&plan->version, comfy_modrinth_version_free);
  g_clear_pointer (&plan->queue, g_ptr_array_unref);
  g_clear_pointer (&plan->files, g_ptr_array_unref);
  g_clear_pointer (&plan->seen, g_hash_table_unref);
  g_clear_object (&plan->cancellable);

  if (plan->operation != NULL)
    comfy_operation_unref (plan->operation);

  g_free (plan->title);
  g_free (plan->replaces);
  g_free (plan);
}

/* What the user is asked to confirm: the mods the install also needs,
 * one per line, and any conflict said in as many words. NULL when there
 * is nothing beside the file itself to say, which is the common case. */
static char *
plan_text (ComfyContentPage *self, PlanRun *plan)
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
          ComfyModrinthDependency *dependency =
            g_ptr_array_index (plan->version->dependencies, i);

          if (dependency->type != comfy_DEPENDENCY_INCOMPATIBLE)
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
  ComfyContentPage *self = comfy_CONTENT_PAGE (user_data);
  PlanRun *plan = g_object_get_data (G_OBJECT (source), "comfy-plan");
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
show_plan (ComfyContentPage *self, PlanRun *plan)
{
  /* An update of the whole list was confirmed once, as a whole: this plan
     is not asked for again, it is simply carried out. So is one pick of a
     confirmed batch of them. */
  if (self->install_model->updating || self->install_model->picking)
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

    g_object_set_data (G_OBJECT (alert), "comfy-plan", plan);
    adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_plan_chosen,
                             g_object_ref (self));
  }
}

/* What @version needs that the plan does not have yet. A dependency
 * that names nothing to fetch stops the plan instead of writing half of
 * it. */
static gboolean
plan_take (PlanRun *plan, ComfyModrinthVersion *version)
{
  ComfyContentPage *self = plan->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) pending = NULL;

  pending = comfy_modrinth_resolve_install (version, NULL, NULL, &error);

  if (pending == NULL)
    {
      if (self != NULL)
        {
           show_error (self, _("This cannot be installed"), error);
          plan_stopped (self);
        }

      return FALSE;
    }

  for (guint i = 0; i < pending->len; i++)
    {
      ComfyModrinthDependency *dependency = g_ptr_array_index (pending, i);

      /* One copy of a project is one file: the second time round it is
       * already in the plan. */
      if (g_hash_table_contains (plan->seen, dependency->project_id))
        continue;

      g_hash_table_add (plan->seen, g_strdup (dependency->project_id));
      g_ptr_array_add (plan->queue,
                       comfy_modrinth_dependency_copy (dependency));
    }

  return TRUE;
}

static void
plan_versions_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  PlanRun *plan = user_data;
  ComfyContentPage *self = plan->page;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  ComfyModrinthVersion *version = NULL;
  InstallFile *file = NULL;

  versions = comfy_modrinth_versions_finish (result, &error);

  /* This answer is in, so the plan does not need the handle that brought
     it; what is left of the plan owns its own work from here. */
  if (plan->operation != NULL)
    {
      comfy_operation_unref (plan->operation);
      plan->operation = NULL;
    }

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
    const char *project_name = NULL;
    ComfyContentFile *installed = installed_project_file (self,
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
      comfy_modrinth_version_free (version);
      plan_free (plan);
      return;
    }

  comfy_modrinth_version_free (version);
  plan_next (plan);
}

/* The install of one version and of everything it requires. */
void
start_plan (ComfyContentPage *self, ComfyModrinthVersion *version,
            const char *title, const char *replaces, gboolean disabled)
{
  PlanRun *plan = g_new0 (PlanRun, 1);

  plan->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &plan->page);
  plan->version = version; /* the plan owns it from here */
  plan->title = g_strdup (title);
  if (replaces == NULL)
    {
      ComfyContentFile *installed = installed_project_file (self,
                                                           version->project_id);

      replaces = installed != NULL ? installed->path : NULL;
      disabled = installed != NULL && !installed->enabled;
    }
  plan->replaces = replaces != NULL ? g_strdup (replaces) : NULL;
  plan->stays_disabled = disabled;
  plan->files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) install_file_free);
  plan->queue = g_ptr_array_new_with_free_func (
      (GDestroyNotify) comfy_modrinth_dependency_free);
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
