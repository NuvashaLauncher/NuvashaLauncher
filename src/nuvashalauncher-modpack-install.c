/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-modpack-private.h"

#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-content-index.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-task-private.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-downloads.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

gboolean
nuvashalauncher_modpack_remove_archive (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return nuvashalauncher_fs_unlink (path, error);
}

gboolean
nuvashalauncher_modpack_install_icon (nuvashalauncherInstanceManager *manager, nuvashalauncherInstance *instance,
                          const char *source_path, GError **error)
{
  g_autofree char *directory = NULL;
  g_autofree char *target = NULL;
  g_autoptr (GFile) source = NULL;
  g_autoptr (GFile) destination = NULL;

  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE_MANAGER (manager), FALSE);
  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (source_path != NULL, FALSE);

  g_object_get (instance, "directory", &directory, NULL);
  target = g_build_filename (directory, "icon.png", NULL);
  source = g_file_new_for_path (source_path);
  destination = g_file_new_for_path (target);

  if (!nuvashalauncher_fs_copy_file (source, destination, G_FILE_COPY_OVERWRITE, NULL,
                         error) ||
      !nuvashalauncher_instance_set_string (instance, "icon", target, error))
    return FALSE;
  return nuvashalauncher_instance_manager_save (manager, error);
}

gboolean
nuvashalauncher_modpack_icon_is_regular (const char *path)
{
  return path != NULL && g_file_test (path, G_FILE_TEST_IS_REGULAR);
}

/* ---------------------------------------------------------- *
 * The tree one install makes, to leave nothing of on failure
 * ---------------------------------------------------------- */

/* ---------------------------------------------------------- *
 * One loader version against what the pack asks for
 * ---------------------------------------------------------- */

/* The names a loader publishes for its installer carry the game first
 * ("1.20.1-47.2.0"), and a pack names the own part only ("47.2.0").
 * Fabric and Quilt publish no installer, and their versions are bare
 * numbers either way. The two spellings are the same version. */
static gboolean
loader_version_matches (nuvashalauncherLoader loader,
                        const char *have,
                        const char *want,
                        const char *game_version)
{
  if (g_strcmp0 (have, want) == 0)
    return TRUE;

  if (game_version != NULL && nuvashalauncher_loader_publishes_an_installer (loader))
    {
      const char separator = loader == nuvashalauncher_LOADER_FORGE ? '-' : '.';
      g_autofree char *prefix =
        g_strdup_printf ("%s%c", game_version, separator);

      if (g_str_has_prefix (have, prefix) &&
          g_strcmp0 (have + strlen (prefix), want) == 0)
        return TRUE;

      if (g_str_has_prefix (want, prefix) &&
          g_strcmp0 (want + strlen (prefix), have) == 0)
        return TRUE;
    }

  return FALSE;
}

/* ---------------------------------------------------------- *
 * The flow of one install
 * ---------------------------------------------------------- */

typedef struct
{
  nuvashalauncherModpack *pack;     /* borrowed: alive until the last finish */
  nuvashalauncherInstanceManager *manager;
  char *name;
  char *target;
  char *temp;
  GCancellable *cancellable;
  GPtrArray *files; /* merged required + chosen optional, borrowed */
  guint total;
  guint current;
  guint pending_index_writes;
  GError *index_error;
  guint attempt;       /* which URL of the current file */
  gboolean saw_disallowed;
  gboolean saw_download_error;
  gboolean saw_hash_error;
  gboolean moved;
  nuvashalauncherModpackProgressFunc progress;
  gpointer progress_data; /* user_data both for the result and progress */
  GTask *task;
  nuvashalauncherTaskScope *scope;
} InstallRun;

static void install_next (InstallRun *run);
static void dispatch_download (InstallRun *run);
static void install_file_done (GObject *, GAsyncResult *, gpointer);
static void install_file_verified (GObject *, GAsyncResult *, gpointer);
static void install_loader_versions (GObject *, GAsyncResult *, gpointer);
static void install_register (InstallRun *run);
static void install_finish_registration (InstallRun *run);
static void install_index_written (GObject *, GAsyncResult *, gpointer);
static void install_fail (InstallRun *run, GError *error);
static void overrides_extracted (GObject *, GAsyncResult *, gpointer);

static char *
modpack_environment (nuvashalauncherModpackEnv environment)
{
  switch (environment)
    {
    case nuvashalauncher_MODPACK_ENV_OPTIONAL:
      return "optional";
    case nuvashalauncher_MODPACK_ENV_UNSUPPORTED:
      return "unsupported";
    case nuvashalauncher_MODPACK_ENV_REQUIRED:
    case nuvashalauncher_MODPACK_ENV_UNSPECIFIED:
    default:
      return "required";
    }
}

static void
seed_manifest_indexes (InstallRun *run)
{
  for (guint i = 0; i < run->files->len; i++)
    {
      nuvashalauncherModpackFile *file = g_ptr_array_index (run->files, i);
      nuvashalauncherContentType type;
      const char *url = file->downloads != NULL && file->downloads->len > 0
                          ? g_ptr_array_index (file->downloads, 0) : NULL;
      g_autofree char *filename = NULL;
      g_autofree char *project_id = NULL;
      g_autofree char *version_id = NULL;
      nuvashalauncherContentIndexEntry entry = { 0 };

      if (g_str_has_prefix (file->path, "mods/"))
        {
          type = nuvashalauncher_CONTENT_MOD;
        }
      else if (g_str_has_prefix (file->path, "resourcepacks/"))
        {
          type = nuvashalauncher_CONTENT_RESOURCE_PACK;
        }
      else if (g_str_has_prefix (file->path, "shaderpacks/"))
        {
          type = nuvashalauncher_CONTENT_SHADER_PACK;
        }
      else if (g_str_has_prefix (file->path, "datapacks/"))
        {
          type = nuvashalauncher_CONTENT_DATAPACK;
        }
      else
        continue;

      filename = g_path_get_basename (file->path);
      if (url != NULL)
        {
          g_autoptr (GUri) uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
          const char *uri_path = uri != NULL ? g_uri_get_path (uri) : NULL;
          g_auto (GStrv) parts = uri_path != NULL
                                   ? g_strsplit (uri_path, "/", -1) : NULL;

          for (guint part = 0; parts != NULL && parts[part] != NULL; part++)
            {
              if (g_strcmp0 (parts[part], "data") == 0 &&
                  parts[part + 1] != NULL)
                project_id = g_strdup (parts[part + 1]);
              if (g_strcmp0 (parts[part], "versions") == 0 &&
                  parts[part + 1] != NULL)
                version_id = g_strdup (parts[part + 1]);
            }
        }
      entry.filename = filename;
      entry.size = (guint64) file->file_size;
      entry.hash = file->sha1;
      entry.hash512 = file->sha512;
      entry.download_url = (char *) url;
      entry.env_client = modpack_environment (file->env_client);
      entry.env_server = modpack_environment (file->env_server);
      entry.provider = "Modrinth";
      entry.project_id = project_id;
      entry.version_id = version_id;

      {
        g_autofree char *path =
          g_build_filename (run->target, file->path, NULL);
        GStatBuf info;

        if (g_stat (path, &info) == 0)
          {
            entry.size = (guint64) info.st_size;
            entry.mtime_usec = (gint64) info.st_mtime * G_USEC_PER_SEC +
                               info.st_mtim.tv_nsec / 1000;
          }
      }

      run->pending_index_writes++;
      nuvashalauncher_task_scope_release_when_done (
        nuvashalauncher_content_index_update_async (run->target, type, &entry,
                                             install_index_written, run),
        run->cancellable);
    }
}

static void
install_index_written (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                       gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_content_index_modify_finish (result, &error) &&
      run->index_error == NULL)
    run->index_error = g_steal_pointer (&error);

  g_assert (run->pending_index_writes > 0);
  run->pending_index_writes--;

  if (run->pending_index_writes == 0)
    {
      if (run->index_error != NULL)
        install_fail (run, g_steal_pointer (&run->index_error));
      else
        install_finish_registration (run);
    }
}

static void
install_run_free (InstallRun *run)
{
  if (run == NULL)
    return;

  g_ptr_array_unref (run->files); /* elements stay with the pack */
  g_clear_object (&run->manager);
  g_clear_object (&run->cancellable);
  g_clear_object (&run->task);
  nuvashalauncher_task_scope_unref (run->scope);
  g_free (run->name);
  g_free (run->target);
  g_free (run->temp);
  g_free (run);
}

/* Everything of ours goes away; anything that was on disk before the
 * install is never touched. */
static void
install_fail (InstallRun *run, GError *error)
{
  if (run->temp != NULL)
    nuvashalauncher_fs_remove_tree (run->temp);

  if (run->moved)
    nuvashalauncher_fs_remove_tree (run->target);

  nuvashalauncher_task_scope_return_error (run->scope, error);
  install_run_free (run);
}

/* One of our temp validation went wrong, in front of any transfer: the
 * only message is a mistake of the caller, and nothing of a tree is
 * touched because none of it is made. */
static void
install_refuse (InstallRun *run, GError *error)
{
  nuvashalauncher_task_scope_return_error_deferred (run->scope, error);
  install_run_free (run);
}

static void
install_loader_versions (GObject *source G_GNUC_UNUSED,
                         GAsyncResult *result,
                         gpointer user_data);
static void install_register (InstallRun *run);

static void
install_after_overrides (InstallRun *run)
{
  nuvashalauncherModpack *pack = run->pack;

  if (pack->loader == nuvashalauncher_LOADER_VANILLA)
    {
      install_register (run);
      return;
    }

  {
    nuvashalauncherTaskScope *listing = nuvashalauncher_loaders_versions_async (pack->loader,
                                                        pack->minecraft_version,
                                                        install_loader_versions,
                                                        run);

    nuvashalauncher_task_scope_follow_cancellable (listing, run->cancellable);
    nuvashalauncher_task_scope_unref_when_done (listing);
  }
}

static void
install_loader_versions (GObject *source G_GNUC_UNUSED,
                         GAsyncResult *result,
                         gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;
  nuvashalauncherModpack *pack = run->pack;
  gboolean found = FALSE;

  versions = nuvashalauncher_loaders_versions_finish (result, &error);

  if (versions == NULL)
    {
      if (g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                           nuvashalauncher_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_clear_error (&error);
          error = g_error_new_literal (nuvashalauncher_MODPACK_ERROR,
                                       nuvashalauncher_MODPACK_ERROR_CANCELLED,
                                       "cancelled");
        }
      else
        {
          g_autofree char *message =
             g_strdup_printf (_("the loader cannot be resolved: %s"),
                             error->message);

          g_clear_error (&error);
          error = g_error_new_literal (nuvashalauncher_MODPACK_ERROR,
                                       nuvashalauncher_MODPACK_ERROR_LOADER, message);
        }

      install_fail (run, g_steal_pointer (&error));
      return;
    }

  for (guint i = 0; i < versions->len && !found; i++)
    {
      nuvashalauncherLoaderVersion *version = g_ptr_array_index (versions, i);

      if (loader_version_matches (pack->loader, version->version,
                                  pack->loader_version, pack->minecraft_version))
        found = TRUE;
    }

  if (!found)
    {
      g_autofree char *message = g_strdup_printf (
        "\"%s\" names \"%s\", and no such version of it runs Minecraft %s",
        pack->name != NULL ? pack->name : "the pack", pack->loader_version,
        pack->minecraft_version != NULL ? pack->minecraft_version : "?");

      g_set_error_literal (&error, nuvashalauncher_MODPACK_ERROR,
                           nuvashalauncher_MODPACK_ERROR_LOADER, message);
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  install_register (run);
}

static void
install_register (InstallRun *run)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *temp = NULL;

  /* Everything of the pack is in its temp directory: rename is the
   * atomic set-in-place, and there is nothing of ours next to the
   * target but that same tree it just became. The temp directory is
   * no longer ours to clean up on a failure once the move is done, so
   * run->temp is emptied as the move starts. */
  temp = g_steal_pointer (&run->temp);

  if (!nuvashalauncher_fs_rename (temp, run->target, &error))
    {
      run->temp = g_steal_pointer (&temp);
      g_prefix_error (&error, "the unpacked pack cannot be moved: ");
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  run->moved = TRUE;

  seed_manifest_indexes (run);
  if (run->pending_index_writes > 0)
    return;

  install_finish_registration (run);
}

static void
install_finish_registration (InstallRun *run)
{
  g_autoptr (nuvashalauncherInstance) instance = NULL;
  g_autoptr (GError) error = NULL;
  nuvashalauncherModpack *pack = run->pack;

  instance = nuvashalauncher_instance_manager_create (run->manager, run->name,
                                          run->target, pack->minecraft_version,
                                          pack->loader, pack->loader_version,
                                          &error);

  if (instance == NULL)
    {
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  g_object_set (instance,
                "modpack-name", pack->name,
                "modpack-summary", pack->summary,
                "modpack-version-id", pack->version_id,
                NULL);
  if (!nuvashalauncher_instance_manager_save (run->manager, &error))
    {
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  if (pack->icon != NULL)
    {
      g_autofree char *icon = g_build_filename (run->target, pack->icon, NULL);

      if (g_file_test (icon, G_FILE_TEST_IS_REGULAR))
        {
          nuvashalauncher_instance_set_string (instance, "icon", icon, &error);
          if (!nuvashalauncher_instance_manager_save (run->manager, &error))
            {
              install_fail (run, g_steal_pointer (&error));
              return;
            }
        }
    }

  nuvashalauncher_task_scope_return_pointer (run->scope, g_object_ref (instance),
                                 g_object_unref);
  install_run_free (run);
}

/* The next file in line, or the overrides once there are none. */
static void
install_next (InstallRun *run)
{
  if (run->current >= run->total)
    {
      if (run->progress != NULL)
        run->progress (run->current, run->total, NULL, run->progress_data);
      nuvashalauncher_task_scope_release_when_done (
        extract_overrides_async (run->pack->archive_path, run->temp,
                                 overrides_extracted, run),
        run->cancellable);
      return;
    }

  run->attempt = 0;
  run->saw_disallowed = FALSE;
  run->saw_download_error = FALSE;
  run->saw_hash_error = FALSE;

  dispatch_download (run);
}

static void
overrides_extracted (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!extract_overrides_finish (result, &error))
    {
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  install_after_overrides (run);
}

static void
dispatch_download (InstallRun *run)
{
  nuvashalauncherModpackFile *file = g_ptr_array_index (run->files, run->current);
  const char *url = NULL;

  while (run->attempt < file->downloads->len)
    {
      url = g_ptr_array_index (file->downloads, run->attempt);

      if (nuvashalauncher_modpack_download_url_allowed (url))
        break;

      run->saw_disallowed = TRUE;
      run->attempt++;
    }

  if (run->attempt >= file->downloads->len)
    {
      g_autoptr (GError) error = NULL;

      if (run->saw_hash_error)
        {
          g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_HASH,
                       "what arrived for \"%s\" did not match the hashes of "
                       "the manifest",
                       file->path);
        }
      else if (run->saw_download_error)
        {
          g_set_error (&error, nuvashalauncher_MODPACK_ERROR,
                       nuvashalauncher_MODPACK_ERROR_DOWNLOAD,
                       "\"%s\" could not be downloaded", file->path);
        }
      else if (run->saw_disallowed)
        {
          g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_URL,
                       "every URL the manifest gives for \"%s\" is from a "
                       "host this launcher cannot ask",
                       file->path);
        }
      else
        {
          g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_DOWNLOAD,
                       "\"%s\" has no download URL", file->path);
        }

      install_fail (run, g_steal_pointer (&error));
      return;
    }

  if (!nuvashalauncher_modpack_path_is_safe (file->path))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_PATH,
                   "the manifest wants \"%s\", which is not a path the "
                   "instance can hold",
                   file->path);
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  {
    g_autofree char *dest = g_build_filename (run->temp, file->path, NULL);
    g_autofree char *parent = g_path_get_dirname (dest);

    g_autoptr (GError) directory_error = NULL;
    if (!nuvashalauncher_fs_make_directory (parent, 0755, &directory_error))
      {
        g_autoptr (GError) error = NULL;

        g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_IO,
                     "a directory under the instance cannot be made: %s",
                     directory_error->message);
        install_fail (run, g_steal_pointer (&error));
        return;
      }

    nuvashalauncher_task_scope_release_when_done (
      nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), url, dest, NULL, 0, NULL, install_file_done,
                               run),
      run->cancellable);
  }
}

static void
install_file_done (GObject *source G_GNUC_UNUSED,
                   GAsyncResult *result,
                   gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      if (g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                           nuvashalauncher_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          install_fail (
            run,
            g_error_new_literal (nuvashalauncher_MODPACK_ERROR,
                                 nuvashalauncher_MODPACK_ERROR_CANCELLED, "cancelled"));
          return;
        }

      run->saw_download_error = TRUE;

      /* The next URL of the same file may work where the last did not. */
      run->attempt++;
      dispatch_download (run);
      return;
    }

  {
    nuvashalauncherModpackFile *file = g_ptr_array_index (run->files, run->current);
    g_autofree char *dest = g_build_filename (run->temp, file->path, NULL);

    nuvashalauncher_task_scope_release_when_done (
      nuvashalauncher_modpack_verify_file_async (file, dest, install_file_verified, run),
      run->cancellable);
  }
}

static void
install_file_verified (GObject *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer user_data)
{
  InstallRun *run = user_data;
  g_autoptr (GError) error = NULL;
  nuvashalauncherModpackFile *file = NULL;
  g_autofree char *dest = NULL;

  if (!nuvashalauncher_modpack_verify_file_finish (result, &error))
    {
      /* A hash that does not match is no good download: wrong in a way
       * a try of the next URL may put right. */
      if (g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                           nuvashalauncher_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          install_fail (
            run,
            g_error_new_literal (nuvashalauncher_MODPACK_ERROR,
                                 nuvashalauncher_MODPACK_ERROR_CANCELLED, "cancelled"));
          return;
        }

      file = g_ptr_array_index (run->files, run->current);
      dest = g_build_filename (run->temp, file->path, NULL);

      nuvashalauncher_fs_unlink (dest, NULL);

      run->saw_hash_error = TRUE;
      run->attempt++;

      dispatch_download (run);
      return;
    }

  file = g_ptr_array_index (run->files, run->current);

  /* The file that arrived has the two hashes: the next file, and what
   * of this one the user sees. */
  run->current++;

  if (run->progress != NULL)
    run->progress (run->current, run->total, file->path, run->progress_data);

  install_next (run);
}

nuvashalauncherTaskScope *
nuvashalauncher_modpack_install_async (nuvashalauncherModpack *modpack,
                           nuvashalauncherInstanceManager *manager,
                           const char *name,
                           const char *directory,
                           const GPtrArray *optional_files,
                           nuvashalauncherModpackProgressFunc progress,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvashalauncher_task_scope_task (scope);
  nuvashalauncherTaskScope *task_scope = scope;
  InstallRun *run = NULL;
  g_autofree char *parent = NULL;

  if (modpack == NULL || manager == NULL || name == NULL || name[0] == '\0')
    {
      nuvashalauncher_task_scope_return_error_deferred (
        task_scope,
        g_error_new_literal (nuvashalauncher_MODPACK_ERROR,
                             nuvashalauncher_MODPACK_ERROR_FORMAT,
                             "the install is missing its modpack, its "
                             "manager or its name"));
      return nuvashalauncher_task_scope_ref (task_scope);
    }

  if (modpack->minecraft_version == NULL)
    {
      nuvashalauncher_task_scope_return_error_deferred (
        task_scope,
        g_error_new_literal (nuvashalauncher_MODPACK_ERROR,
                             nuvashalauncher_MODPACK_ERROR_LOADER,
                             "the dependencies were never read: call "
                             "nuvashalauncher_modpack_resolve() first"));
      return nuvashalauncher_task_scope_ref (task_scope);
    }

  run = g_new0 (InstallRun, 1);

  run->pack = modpack;
  run->manager = g_object_ref (manager);
  run->name = g_strdup (name);
  run->cancellable = g_object_ref (nuvashalauncher_task_scope_get_cancellable (task_scope));
  run->progress = progress;
  run->progress_data = user_data;
  run->files = g_ptr_array_new ();
  run->task = g_object_ref (task);
  run->scope = nuvashalauncher_task_scope_ref (task_scope);

  /* The target is what the manager would choose for the name, unless
   * the caller says where the instance is to go. */
  if (directory != NULL)
    run->target = g_strdup (directory);
  else
    run->target = nuvashalauncher_instance_manager_default_directory (manager, name);

  if (run->target == NULL || !g_path_is_absolute (run->target))
    {
      g_autoptr (GError) error = NULL;

      g_set_error_literal (&error, nuvashalauncher_MODPACK_ERROR,
                           nuvashalauncher_MODPACK_ERROR_IO,
                           "the directory of the instance is not usable");
      install_refuse (run, g_steal_pointer (&error));
      return nuvashalauncher_task_scope_ref (task_scope);
    }

  /* What already runs in the chosen place is user data: never touch it. */
  if (g_file_test (run->target, G_FILE_TEST_EXISTS))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_IO,
                   "a directory already exists at \"%s\"", run->target);
      install_refuse (run, g_steal_pointer (&error));
      return nuvashalauncher_task_scope_ref (task_scope);
    }

  {
    g_autoptr (GPtrArray) required =
      nuvashalauncher_modpack_required_client_files (modpack);

    for (guint i = 0; i < required->len; i++)
      g_ptr_array_add (run->files, g_ptr_array_index (required, i));

    if (optional_files != NULL)
      {
        for (guint i = 0; i < optional_files->len; i++)
          g_ptr_array_add (run->files, g_ptr_array_index (optional_files, i));
      }
  }

  run->total = run->files->len;
  run->current = 0;
  run->attempt = 0;

  /* Room for the tree the install is built in, next to where it ends
   * up: the final move is one rename on one filesystem. The parent is
   * made if it has to be; what is made stays. */
  parent = g_path_get_dirname (run->target);

  g_autoptr (GError) parent_error = NULL;
  if (!nuvashalauncher_fs_make_directory (parent, 0755, &parent_error))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_IO,
                   "a directory under the instance cannot be made: %s",
                   parent_error->message);
      install_refuse (run, g_steal_pointer (&error));
      return nuvashalauncher_task_scope_ref (task_scope);
    }

  g_autoptr (GError) temp_error = NULL;
  run->temp = nuvashalauncher_fs_make_temporary_directory (parent, ".nuvashalauncher-modpack-XXXXXX",
                                               &temp_error);

  if (run->temp == NULL)
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_IO,
                   "the temporary directory cannot be made: %s",
                   temp_error->message);
      install_refuse (run, g_steal_pointer (&error));
      return nuvashalauncher_task_scope_ref (task_scope);
    }

  /* mkdtemp creates a private directory on supported POSIX systems, but
   * enforce that contract explicitly: extraction and download paths below
   * must never be writable by another local user. */
  if (g_chmod (run->temp, 0700) != 0)
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_IO,
                   "the temporary directory cannot be secured: %s",
                   g_strerror (errno));
      install_refuse (run, g_steal_pointer (&error));
      return nuvashalauncher_task_scope_ref (task_scope);
    }

  /* install_next and not dispatch_download: a pack may name no files
   * at all to download, and then the first thing it wants is its
   * overrides, not a file that is not there. */
  install_next (run);
  return nuvashalauncher_task_scope_ref (task_scope);
}

nuvashalauncherInstance *
nuvashalauncher_modpack_install_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
