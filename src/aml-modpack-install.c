/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "aml-modpack.h"
#include "aml-modpack-private.h"

#include "aml-i18n.h"
#include "aml-content-index.h"
#include "aml-fs.h"
#include "aml-task-private.h"
#include "aml-loaders.h"
#include "aml-downloads.h"

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
aml_modpack_remove_archive (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  return aml_fs_unlink (path, error);
}

gboolean
aml_modpack_install_icon (AmlInstanceManager *manager, AmlInstance *instance,
                          const char *source_path, GError **error)
{
  g_autofree char *directory = NULL;
  g_autofree char *target = NULL;
  g_autoptr (GFile) source = NULL;
  g_autoptr (GFile) destination = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (manager), FALSE);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (source_path != NULL, FALSE);

  g_object_get (instance, "directory", &directory, NULL);
  target = g_build_filename (directory, "icon.png", NULL);
  source = g_file_new_for_path (source_path);
  destination = g_file_new_for_path (target);

  if (!aml_fs_copy_file (source, destination, G_FILE_COPY_OVERWRITE, NULL,
                         error) ||
      !aml_instance_set_string (instance, "icon", target, error))
    return FALSE;
  return aml_instance_manager_save (manager, error);
}

gboolean
aml_modpack_icon_is_regular (const char *path)
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
loader_version_matches (AmlLoader loader,
                        const char *have,
                        const char *want,
                        const char *game_version)
{
  if (g_strcmp0 (have, want) == 0)
    return TRUE;

  if (game_version != NULL && aml_loader_publishes_an_installer (loader))
    {
      const char separator = loader == AML_LOADER_FORGE ? '-' : '.';
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
  AmlModpack *pack;     /* borrowed: alive until the last finish */
  AmlInstanceManager *manager;
  char *name;
  char *target;
  char *temp;
  GCancellable *cancellable;
  GPtrArray *files; /* merged required + chosen optional, borrowed */
  guint total;
  guint current;
  guint attempt;       /* which URL of the current file */
  gboolean saw_disallowed;
  gboolean saw_download_error;
  gboolean saw_hash_error;
  gboolean moved;
  AmlModpackProgressFunc progress;
  gpointer progress_data; /* user_data both for the result and progress */
  GTask *task;
  AmlTaskScope *scope;
} InstallRun;

static void install_next (InstallRun *run);
static void dispatch_download (InstallRun *run);
static void install_file_done (GObject *, GAsyncResult *, gpointer);
static void install_file_verified (GObject *, GAsyncResult *, gpointer);
static void install_loader_versions (GObject *, GAsyncResult *, gpointer);
static void install_register (InstallRun *run);
static void overrides_extracted (GObject *, GAsyncResult *, gpointer);

static void
seed_manifest_indexes (InstallRun *run)
{
  for (guint i = 0; i < run->files->len; i++)
    {
      AmlModpackFile *file = g_ptr_array_index (run->files, i);
      AmlContentType type;
      const char *url = file->downloads != NULL && file->downloads->len > 0
                          ? g_ptr_array_index (file->downloads, 0) : NULL;
      g_autofree char *filename = NULL;
      g_autofree char *project_id = NULL;
      g_autofree char *version_id = NULL;
      AmlContentIndexEntry entry = { 0 };

      if (g_str_has_prefix (file->path, "mods/"))
        {
          type = AML_CONTENT_MOD;
        }
      else if (g_str_has_prefix (file->path, "resourcepacks/"))
        {
          type = AML_CONTENT_RESOURCE_PACK;
        }
      else if (g_str_has_prefix (file->path, "shaderpacks/"))
        {
          type = AML_CONTENT_SHADER_PACK;
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
      entry.hash = file->sha1;
      entry.hash512 = file->sha512;
      entry.download_url = (char *) url;
      entry.env_client = "required";
      entry.env_server = type == AML_CONTENT_MOD ? "required" : "unsupported";
      entry.provider = "Modrinth";
      entry.project_id = project_id;
      entry.version_id = version_id;
      aml_task_scope_release_when_done (
        aml_content_index_update_async (run->target, type, &entry, NULL, NULL),
        run->cancellable);
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
  aml_task_scope_unref (run->scope);
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
    aml_fs_remove_tree (run->temp);

  if (run->moved)
    aml_fs_remove_tree (run->target);

  aml_task_scope_return_error (run->scope, error);
  install_run_free (run);
}

/* One of our temp validation went wrong, in front of any transfer: the
 * only message is a mistake of the caller, and nothing of a tree is
 * touched because none of it is made. */
static void
install_refuse (InstallRun *run, GError *error)
{
  aml_task_scope_return_error_deferred (run->scope, error);
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
  AmlModpack *pack = run->pack;

  if (pack->loader == AML_LOADER_VANILLA)
    {
      install_register (run);
      return;
    }

  {
    AmlTaskScope *listing = aml_loaders_versions_async (pack->loader,
                                                        pack->minecraft_version,
                                                        install_loader_versions,
                                                        run);

    aml_task_scope_follow_cancellable (listing, run->cancellable);
    aml_task_scope_unref_when_done (listing);
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
  AmlModpack *pack = run->pack;
  gboolean found = FALSE;

  versions = aml_loaders_versions_finish (result, &error);

  if (versions == NULL)
    {
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_clear_error (&error);
          error = g_error_new_literal (AML_MODPACK_ERROR,
                                       AML_MODPACK_ERROR_CANCELLED,
                                       "cancelled");
        }
      else
        {
          g_autofree char *message =
             g_strdup_printf (_("the loader cannot be resolved: %s"),
                             error->message);

          g_clear_error (&error);
          error = g_error_new_literal (AML_MODPACK_ERROR,
                                       AML_MODPACK_ERROR_LOADER, message);
        }

      install_fail (run, g_steal_pointer (&error));
      return;
    }

  for (guint i = 0; i < versions->len && !found; i++)
    {
      AmlLoaderVersion *version = g_ptr_array_index (versions, i);

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

      g_set_error_literal (&error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_LOADER, message);
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  install_register (run);
}

static void
install_register (InstallRun *run)
{
  g_autoptr (AmlInstance) instance = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *temp = NULL;
  AmlModpack *pack = run->pack;

  /* Everything of the pack is in its temp directory: rename is the
   * atomic set-in-place, and there is nothing of ours next to the
   * target but that same tree it just became. The temp directory is
   * no longer ours to clean up on a failure once the move is done, so
   * run->temp is emptied as the move starts. */
  temp = g_steal_pointer (&run->temp);

  if (!aml_fs_rename (temp, run->target, &error))
    {
      run->temp = g_steal_pointer (&temp);
      g_prefix_error (&error, "the unpacked pack cannot be moved: ");
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  run->moved = TRUE;

  instance = aml_instance_manager_create (run->manager, run->name,
                                          run->target, pack->minecraft_version,
                                          pack->loader, pack->loader_version,
                                          &error);

  if (instance == NULL)
    {
      aml_fs_remove_tree (run->target);
      install_fail (run, g_steal_pointer (&error));
      return;
    }

  if (pack->icon != NULL)
    {
      g_autofree char *icon = g_build_filename (run->target, pack->icon, NULL);

      if (g_file_test (icon, G_FILE_TEST_IS_REGULAR))
        {
          aml_instance_set_string (instance, "icon", icon, &error);
          if (!aml_instance_manager_save (run->manager, &error))
            {
              aml_fs_remove_tree (run->target);
              install_fail (run, g_steal_pointer (&error));
              return;
            }
        }
    }

  /* The manifest gives us the hashes and download URLs immediately. Project
   * names, dependencies and icons are filled in later by the content page's
   * Modrinth hash lookup. */
  seed_manifest_indexes (run);

  aml_task_scope_return_pointer (run->scope, g_object_ref (instance),
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
      aml_task_scope_release_when_done (
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
  AmlModpackFile *file = g_ptr_array_index (run->files, run->current);
  const char *url = NULL;

  while (run->attempt < file->downloads->len)
    {
      url = g_ptr_array_index (file->downloads, run->attempt);

      if (aml_modpack_download_url_allowed (url))
        break;

      run->saw_disallowed = TRUE;
      run->attempt++;
    }

  if (run->attempt >= file->downloads->len)
    {
      g_autoptr (GError) error = NULL;

      if (run->saw_hash_error)
        {
          g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_HASH,
                       "what arrived for \"%s\" did not match the hashes of "
                       "the manifest",
                       file->path);
        }
      else if (run->saw_download_error)
        {
          g_set_error (&error, AML_MODPACK_ERROR,
                       AML_MODPACK_ERROR_DOWNLOAD,
                       "\"%s\" could not be downloaded", file->path);
        }
      else if (run->saw_disallowed)
        {
          g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_URL,
                       "every URL the manifest gives for \"%s\" is from a "
                       "host this launcher cannot ask",
                       file->path);
        }
      else
        {
          g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_DOWNLOAD,
                       "\"%s\" has no download URL", file->path);
        }

      install_fail (run, g_steal_pointer (&error));
      return;
    }

  if (!aml_modpack_path_is_safe (file->path))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
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
    if (!aml_fs_make_directory (parent, 0755, &directory_error))
      {
        g_autoptr (GError) error = NULL;

        g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                     "a directory under the instance cannot be made: %s",
                     directory_error->message);
        install_fail (run, g_steal_pointer (&error));
        return;
      }

    aml_task_scope_release_when_done (
      aml_download_file_async (aml_downloads_get_service (), url, dest, NULL, 0, NULL, install_file_done,
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

  if (!aml_download_file_finish (result, &error))
    {
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          install_fail (
            run,
            g_error_new_literal (AML_MODPACK_ERROR,
                                 AML_MODPACK_ERROR_CANCELLED, "cancelled"));
          return;
        }

      run->saw_download_error = TRUE;

      /* The next URL of the same file may work where the last did not. */
      run->attempt++;
      dispatch_download (run);
      return;
    }

  {
    AmlModpackFile *file = g_ptr_array_index (run->files, run->current);
    g_autofree char *dest = g_build_filename (run->temp, file->path, NULL);

    aml_task_scope_release_when_done (
      aml_modpack_verify_file_async (file, dest, install_file_verified, run),
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
  AmlModpackFile *file = NULL;
  g_autofree char *dest = NULL;

  if (!aml_modpack_verify_file_finish (result, &error))
    {
      /* A hash that does not match is no good download: wrong in a way
       * a try of the next URL may put right. */
      if (g_error_matches (error, AML_DOWNLOAD_ERROR,
                           AML_DOWNLOAD_ERROR_CANCELLED) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          install_fail (
            run,
            g_error_new_literal (AML_MODPACK_ERROR,
                                 AML_MODPACK_ERROR_CANCELLED, "cancelled"));
          return;
        }

      file = g_ptr_array_index (run->files, run->current);
      dest = g_build_filename (run->temp, file->path, NULL);

      aml_fs_unlink (dest, NULL);

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

AmlTaskScope *
aml_modpack_install_async (AmlModpack *modpack,
                           AmlInstanceManager *manager,
                           const char *name,
                           const char *directory,
                           const GPtrArray *optional_files,
                           AmlModpackProgressFunc progress,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  AmlTaskScope *task_scope = scope;
  InstallRun *run = NULL;
  g_autofree char *parent = NULL;

  if (modpack == NULL || manager == NULL || name == NULL || name[0] == '\0')
    {
      aml_task_scope_return_error_deferred (
        task_scope,
        g_error_new_literal (AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_FORMAT,
                             "the install is missing its modpack, its "
                             "manager or its name"));
      return aml_task_scope_ref (task_scope);
    }

  if (modpack->minecraft_version == NULL)
    {
      aml_task_scope_return_error_deferred (
        task_scope,
        g_error_new_literal (AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_LOADER,
                             "the dependencies were never read: call "
                             "aml_modpack_resolve() first"));
      return aml_task_scope_ref (task_scope);
    }

  run = g_new0 (InstallRun, 1);

  run->pack = modpack;
  run->manager = g_object_ref (manager);
  run->name = g_strdup (name);
  run->cancellable = g_object_ref (aml_task_scope_get_cancellable (task_scope));
  run->progress = progress;
  run->progress_data = user_data;
  run->files = g_ptr_array_new ();
  run->task = g_object_ref (task);
  run->scope = aml_task_scope_ref (task_scope);

  /* The target is what the manager would choose for the name, unless
   * the caller says where the instance is to go. */
  if (directory != NULL)
    run->target = g_strdup (directory);
  else
    run->target = aml_instance_manager_default_directory (manager, name);

  if (run->target == NULL || !g_path_is_absolute (run->target))
    {
      g_autoptr (GError) error = NULL;

      g_set_error_literal (&error, AML_MODPACK_ERROR,
                           AML_MODPACK_ERROR_IO,
                           "the directory of the instance is not usable");
      install_refuse (run, g_steal_pointer (&error));
      return aml_task_scope_ref (task_scope);
    }

  /* What already runs in the chosen place is user data: never touch it. */
  if (g_file_test (run->target, G_FILE_TEST_EXISTS))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "a directory already exists at \"%s\"", run->target);
      install_refuse (run, g_steal_pointer (&error));
      return aml_task_scope_ref (task_scope);
    }

  {
    g_autoptr (GPtrArray) required =
      aml_modpack_required_client_files (modpack);

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
  if (!aml_fs_make_directory (parent, 0755, &parent_error))
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "a directory under the instance cannot be made: %s",
                   parent_error->message);
      install_refuse (run, g_steal_pointer (&error));
      return aml_task_scope_ref (task_scope);
    }

  g_autoptr (GError) temp_error = NULL;
  run->temp = aml_fs_make_temporary_directory (parent, ".aml-modpack-XXXXXX",
                                               &temp_error);

  if (run->temp == NULL)
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "the temporary directory cannot be made: %s",
                   temp_error->message);
      install_refuse (run, g_steal_pointer (&error));
      return aml_task_scope_ref (task_scope);
    }

  /* mkdtemp creates a private directory on supported POSIX systems, but
   * enforce that contract explicitly: extraction and download paths below
   * must never be writable by another local user. */
  if (g_chmod (run->temp, 0700) != 0)
    {
      g_autoptr (GError) error = NULL;

      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "the temporary directory cannot be secured: %s",
                   g_strerror (errno));
      install_refuse (run, g_steal_pointer (&error));
      return aml_task_scope_ref (task_scope);
    }

  /* install_next and not dispatch_download: a pack may name no files
   * at all to download, and then the first thing it wants is its
   * overrides, not a file that is not there. */
  install_next (run);
  return aml_task_scope_ref (task_scope);
}

AmlInstance *
aml_modpack_install_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

typedef struct {
  char *instance_directory;
  char *archive_path;
  char *temporary_archive;
  char *manifest;
  char *icon_path;
  GHashTable *indexed_paths;
  gboolean include_transient;
  AmlModpackExportProgressFunc progress;
  gpointer progress_data;
  guint64 done;
  guint64 total;

  /* Borrowed. This run is the task's own data and the scope owns that
   * task, so a reference here would make the two hold each other up. */
  AmlTaskScope *scope;
} ExportRun;

static void
export_run_free (ExportRun *run)
{
  g_free (run->instance_directory);
  g_free (run->archive_path);
  g_free (run->temporary_archive);
  g_free (run->manifest);
  g_free (run->icon_path);
  g_clear_pointer (&run->indexed_paths, g_hash_table_unref);
  g_free (run);
}

typedef struct {
  AmlModpackExportProgressFunc callback;
  gpointer data;
  guint64 done;
  guint64 total;
  char *detail;
} ExportProgress;

static gboolean
export_progress_dispatch (gpointer user_data)
{
  ExportProgress *progress = user_data;

  progress->callback (progress->done, progress->total, progress->detail,
                      progress->data);
  g_free (progress->detail);
  g_free (progress);
  return G_SOURCE_REMOVE;
}

static void
export_progress (ExportRun *run, guint64 done, guint64 total,
                 const char *detail)
{
  ExportProgress *progress;

  if (run->progress == NULL)
    return;

  progress = g_new0 (ExportProgress, 1);
  progress->callback = run->progress;
  progress->data = run->progress_data;
  progress->done = done;
  progress->total = total;
  progress->detail = g_strdup (detail);
  g_main_context_invoke (NULL, export_progress_dispatch, progress);
}

static gboolean
export_file (ExportRun *run, struct archive *archive, const char *source,
             const char *target,
             GCancellable *cancellable, GError **error)
{
  GStatBuf stat_buf;
  FILE *input = NULL;
  struct archive_entry *entry = NULL;
  char buffer[64 * 1024];
  size_t length;

  if (g_stat (source, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "cannot read instance file \"%s\": %s", source,
                   g_strerror (errno));
      return FALSE;
    }

  input = g_fopen (source, "rb");
  if (input == NULL)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "cannot open instance file \"%s\": %s", source,
                   g_strerror (errno));
      return FALSE;
    }

  entry = archive_entry_new ();
  archive_entry_set_pathname (entry, target);
  archive_entry_set_size (entry, (la_int64_t) stat_buf.st_size);
  archive_entry_set_filetype (entry, AE_IFREG);
  archive_entry_set_perm (entry, 0644);

  if (archive_write_header (archive, entry) != ARCHIVE_OK)
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "cannot write \"%s\": %s", target,
                   archive_error_string (archive));
      archive_entry_free (entry);
      fclose (input);
      return FALSE;
    }

  while ((length = fread (buffer, 1, sizeof buffer, input)) > 0)
    {
      if (g_cancellable_is_cancelled (cancellable))
        {
          g_set_error_literal (error, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_CANCELLED, "cancelled");
          archive_entry_free (entry);
          fclose (input);
          return FALSE;
        }

      if (archive_write_data (archive, buffer, length) != (la_ssize_t) length)
      {
        g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                     "cannot write \"%s\": %s", target,
                     archive_error_string (archive));
        archive_entry_free (entry);
        fclose (input);
        return FALSE;
      }
    }

  if (ferror (input))
    {
      g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                   "cannot read instance file \"%s\": %s", source,
                   g_strerror (errno));
      archive_entry_free (entry);
      fclose (input);
      return FALSE;
    }

  fclose (input);
  archive_entry_free (entry);
  run->done++;
  export_progress (run, run->done, run->total, _("Writing archive"));
  return TRUE;
}

static gboolean
export_path_is_transient (const char *relative)
{
  static const char *names[] = {
    "logs", "crash-reports", "screenshots", "cache", "downloads", "debug"
  };
  const char *end = strchr (relative, G_DIR_SEPARATOR);
  gsize length = end != NULL ? (gsize) (end - relative) : strlen (relative);

  if (g_str_has_prefix (relative, ".index") ||
      strstr (relative, G_DIR_SEPARATOR_S ".index") != NULL)
    return TRUE;

  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (strlen (names[i]) == length &&
        strncmp (relative, names[i], length) == 0)
      return TRUE;

  return FALSE;
}

static gboolean
export_directory (ExportRun *run, struct archive *archive, const char *root,
                  const char *relative, GHashTable *indexed_paths,
                  gboolean include_transient, GCancellable *cancellable,
                  GError **error)
{
  g_autofree char *directory = g_build_filename (root, relative, NULL);
  g_autoptr (GDir) dir = g_dir_open (directory, 0, error);
  const char *name = NULL;

  if (dir == NULL)
    return FALSE;

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      if (g_cancellable_is_cancelled (cancellable))
        {
          g_set_error_literal (error, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_CANCELLED, "cancelled");
          return FALSE;
        }

      g_autofree char *source = g_build_filename (directory, name, NULL);
      g_autofree char *child_relative =
        g_build_filename (relative, name, NULL);
      GStatBuf stat_buf;

      if (g_str_has_prefix (child_relative, ".index") ||
          strstr (child_relative, G_DIR_SEPARATOR_S ".index") != NULL)
        continue;
      if (!include_transient && export_path_is_transient (child_relative))
        continue;

      if (g_lstat (source, &stat_buf) != 0)
        {
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                       "cannot inspect instance file \"%s\": %s", source,
                       g_strerror (errno));
          return FALSE;
        }

      if (S_ISDIR (stat_buf.st_mode))
        {
          if (!export_directory (run, archive, root, child_relative,
                                 indexed_paths, include_transient, cancellable,
                                 error))
            return FALSE;
        }
      else if (S_ISREG (stat_buf.st_mode))
        {
          g_autofree char *target =
            g_build_filename ("overrides", child_relative, NULL);

          /* The instance icon is written explicitly below so it is also
           * available to PrismLauncher-compatible importers. Do not emit
           * the same source while walking the instance, or the ZIP gets
           * duplicate overrides/icon.png entries. */
          if (run->icon_path != NULL &&
              g_strcmp0 (source, run->icon_path) == 0)
            continue;

          if (g_hash_table_contains (indexed_paths, child_relative))
            continue;

          if (!export_file (run, archive, source, target, cancellable, error))
            return FALSE;
        }
      else if (S_ISLNK (stat_buf.st_mode))
        {
          g_set_error (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_PATH,
                       "cannot export symlink \"%s\"", source);
          return FALSE;
        }
    }

  return TRUE;
}

/* Add installed content whose AML index has both hashes and its original
 * Modrinth URL. Files without complete metadata remain overrides, which is
 * still a valid and lossless export. */
static gboolean
export_indexed_files (ExportRun *run, GCancellable *cancellable, GError **error)
{
  static const char *folders[] = { "mods", "resourcepacks", "shaderpacks" };
  g_autoptr (JsonParser) manifest_parser = json_parser_new ();
  JsonNode *manifest_root = NULL;
  JsonObject *manifest = NULL;
  JsonArray *files = NULL;

  if (!json_parser_load_from_data (manifest_parser, run->manifest, -1, error))
    return FALSE;

  manifest_root = json_parser_get_root (manifest_parser);
  manifest = json_node_get_object (manifest_root);
  files = json_object_get_array_member (manifest, "files");

  for (guint folder_index = 0; folder_index < G_N_ELEMENTS (folders);
       folder_index++)
    {
      if (g_cancellable_is_cancelled (cancellable))
        {
          g_set_error_literal (error, AML_MODPACK_ERROR,
                               AML_MODPACK_ERROR_CANCELLED, "cancelled");
          return FALSE;
        }

      g_autoptr (GError) index_error = NULL;
      g_autoptr (GPtrArray) entries =
        aml_content_index_load_sync (run->instance_directory,
                                     folder_index, &index_error);

      if (entries == NULL)
        continue;

      for (guint i = 0; i < entries->len; i++)
        {
          if (g_cancellable_is_cancelled (cancellable))
            {
              g_set_error_literal (error, AML_MODPACK_ERROR,
                                   AML_MODPACK_ERROR_CANCELLED, "cancelled");
              return FALSE;
            }

          AmlContentIndexEntry *entry = g_ptr_array_index (entries, i);
          const char *filename = NULL;
          const char *sha1 = NULL;
          const char *sha512 = NULL;
          const char *url = NULL;
          const char *env_client = NULL;
          const char *env_server = NULL;
          g_autofree char *source = NULL;
          g_autofree char *disabled_source = NULL;
          gboolean disabled = FALSE;
          GStatBuf stat_buf;
          JsonObject *file = NULL;
          JsonObject *hashes = NULL;
          JsonArray *downloads = NULL;

          filename = entry->filename;
          sha1 = entry->hash;
          sha512 = entry->hash512;
          url = entry->download_url;
          env_client = entry->env_client;
          env_server = entry->env_server;
          source = g_build_filename (run->instance_directory, folders[folder_index],
                                     filename, NULL);

          if (g_stat (source, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
            {
              disabled_source = g_strconcat (source, ".disabled", NULL);
              if (g_stat (disabled_source, &stat_buf) == 0 &&
                  S_ISREG (stat_buf.st_mode))
                {
                  g_free (source);
                  source = g_steal_pointer (&disabled_source);
                  disabled = TRUE;
                }
            }

          if (*filename == '\0' || *sha1 == '\0' || *sha512 == '\0' ||
              *url == '\0' || !aml_modpack_download_url_allowed (url) ||
              g_stat (source, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
            continue;

          g_autofree char *relative =
            g_build_filename (folders[folder_index], filename, NULL);

          file = json_object_new ();
          hashes = json_object_new ();
          downloads = json_array_new ();
          json_object_set_string_member (file, "path", relative);
          json_object_set_string_member (hashes, "sha1", sha1);
          json_object_set_string_member (hashes, "sha512", sha512);
          json_object_set_object_member (file, "hashes", hashes);
          json_array_add_string_element (downloads, url);
          json_object_set_array_member (file, "downloads", downloads);
          {
            JsonObject *env = json_object_new ();
            json_object_set_string_member (env, "client",
                                           disabled ? "optional" : env_client);
            json_object_set_string_member (env, "server",
                                           disabled ? "optional" : env_server);
            json_object_set_object_member (file, "env", env);
          }
          json_object_set_int_member (file, "fileSize", stat_buf.st_size);
          json_array_add_object_element (files, file);
          g_hash_table_add (run->indexed_paths,
                            g_build_filename (folders[folder_index], filename,
                                              NULL));
          if (disabled)
            g_hash_table_add (run->indexed_paths,
                              g_strconcat (folders[folder_index], G_DIR_SEPARATOR_S,
                                           filename, ".disabled", NULL));
        }
    }

  {
    g_autoptr (JsonGenerator) generator = json_generator_new ();
    g_autofree char *data = NULL;

    json_generator_set_root (generator, manifest_root);
    data = json_generator_to_data (generator, NULL);
    g_free (run->manifest);
    run->manifest = g_steal_pointer (&data);
  }

  return TRUE;
}

static guint64
export_count_directory (const char *root, const char *relative,
                        GHashTable *indexed_paths, gboolean include_transient)
{
  g_autofree char *directory = g_build_filename (root, relative, NULL);
  g_autoptr (GDir) dir = g_dir_open (directory, 0, NULL);
  const char *name = NULL;
  guint64 total = 0;

  if (dir == NULL)
    return 0;

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *source = g_build_filename (directory, name, NULL);
      g_autofree char *child = g_build_filename (relative, name, NULL);
      GStatBuf stat_buf;

      if (g_str_has_prefix (child, ".index") ||
          strstr (child, G_DIR_SEPARATOR_S ".index") != NULL)
        continue;
      if (!include_transient && export_path_is_transient (child))
        continue;
      if (g_lstat (source, &stat_buf) != 0)
        continue;
      if (S_ISDIR (stat_buf.st_mode))
        total += export_count_directory (root, child, indexed_paths,
                                         include_transient);
      else if (S_ISREG (stat_buf.st_mode) &&
               !g_hash_table_contains (indexed_paths, child))
        total++;
    }

  return total;
}

static void
export_thread (GTask *task G_GNUC_UNUSED,
                gpointer source_object G_GNUC_UNUSED,
               gpointer task_data, GCancellable *cancellable)
{
  ExportRun *run = task_data;
  g_autoptr (GError) error = NULL;
  struct archive *archive = NULL;
  struct archive_entry *manifest_entry = NULL;
  gsize manifest_length = 0;
  g_autofree char *canonical_root =
    g_canonicalize_filename (run->instance_directory, NULL);
  g_autofree char *canonical_archive =
    g_canonicalize_filename (run->archive_path, NULL);
  g_autofree char *root_prefix =
    g_strconcat (canonical_root, G_DIR_SEPARATOR_S, NULL);

  if (g_cancellable_is_cancelled (cancellable))
    {
      aml_task_scope_return_error (
        run->scope,
        g_error_new_literal (AML_MODPACK_ERROR,
                             AML_MODPACK_ERROR_CANCELLED, "cancelled"));
      return;
    }

  if (!export_indexed_files (run, cancellable, &error))
    {
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      return;
    }

  run->total = g_hash_table_size (run->indexed_paths) +
               export_count_directory (canonical_root, "", run->indexed_paths,
                                       run->include_transient) + 1;
  if (run->icon_path != NULL)
    run->total++;
  export_progress (run, 0, run->total, _("Writing archive"));

  manifest_length = strlen (run->manifest);

  if (g_str_has_prefix (canonical_archive, root_prefix))
    {
      aml_task_scope_return_error (
        run->scope,
        g_error_new_literal (AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                             "the export file cannot be inside the instance"));
      return;
    }

  {
    g_autofree char *parent = g_path_get_dirname (run->archive_path);
    int fd = -1;

    g_autoptr (GError) temporary_error = NULL;
    fd = aml_fs_open_temporary_file (parent, ".aml-modpack-XXXXXX",
                                     &run->temporary_archive,
                                     &temporary_error);
    if (fd < 0)
      {
        aml_task_scope_return_error (
          run->scope,
          g_error_new (AML_MODPACK_ERROR, AML_MODPACK_ERROR_IO,
                       "cannot create a temporary archive: %s",
                       temporary_error->message));
        return;
      }
    close (fd);
  }

  archive = archive_write_new ();
  archive_write_set_format_zip (archive);

  if (archive_write_open_filename (archive, run->temporary_archive) != ARCHIVE_OK)
    {
      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "cannot create \"%s\": %s", run->archive_path,
                   archive_error_string (archive));
      archive_write_free (archive);
      aml_fs_unlink (run->temporary_archive, NULL);
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      return;
    }

  manifest_entry = archive_entry_new ();
  archive_entry_set_pathname (manifest_entry, "modrinth.index.json");
  archive_entry_set_size (manifest_entry, (la_int64_t) manifest_length);
  archive_entry_set_filetype (manifest_entry, AE_IFREG);
  archive_entry_set_perm (manifest_entry, 0644);

  if (archive_write_header (archive, manifest_entry) != ARCHIVE_OK ||
      archive_write_data (archive, run->manifest, manifest_length) !=
        (la_ssize_t) manifest_length)
    {
      g_set_error (&error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_ARCHIVE,
                   "cannot write the modpack manifest: %s",
                   archive_error_string (archive));
      archive_entry_free (manifest_entry);
      archive_write_free (archive);
      aml_fs_unlink (run->temporary_archive, NULL);
       aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      return;
    }

  run->done = 1;
      export_progress (run, run->done, run->total, _("Writing archive"));

  if (!export_directory (run, archive, canonical_root, "", run->indexed_paths,
                         run->include_transient, cancellable, &error) ||
      (run->icon_path != NULL &&
       !export_file (run, archive, run->icon_path, "overrides/icon.png",
                     cancellable,
                     &error)) ||
      g_cancellable_is_cancelled (cancellable) ||
      archive_write_close (archive) != ARCHIVE_OK)
    {
      if (error == NULL)
        g_set_error (&error, AML_MODPACK_ERROR,
                     g_cancellable_is_cancelled (cancellable)
                       ? AML_MODPACK_ERROR_CANCELLED
                       : AML_MODPACK_ERROR_ARCHIVE,
                     g_cancellable_is_cancelled (cancellable)
                       ? "cancelled" : "cannot finish the modpack archive");
      archive_entry_free (manifest_entry);
      archive_write_free (archive);
      aml_fs_unlink (run->temporary_archive, NULL);
       aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      return;
    }

  archive_entry_free (manifest_entry);
  archive_write_free (archive);

  if (!aml_fs_rename (run->temporary_archive, run->archive_path, &error))
    {
      g_prefix_error (&error, "cannot publish \"%s\": ",
                      run->archive_path);
      aml_fs_unlink (run->temporary_archive, NULL);
      aml_task_scope_return_error (run->scope, g_steal_pointer (&error));
      return;
    }

  export_progress (run, run->total, run->total, _("Export complete"));
  aml_task_scope_return_boolean (run->scope, TRUE);
}

static char *
export_manifest (AmlInstance *instance, const char *pack_name,
                 const char *summary, const char *version_id, GError **error)
{
  g_autofree char *name = NULL;
  g_autofree char *version = NULL;
  g_autofree char *loader_version = NULL;
  g_autofree char *icon = NULL;
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *result = NULL;
  AmlLoader loader = AML_LOADER_VANILLA;
  gsize length = 0;

  g_object_get (instance, "name", &name, "minecraft_version", &version,
                "loader", &loader, "loader_version", &loader_version,
                "icon", &icon, NULL);

  if (version == NULL || *version == '\0')
    {
      g_set_error_literal (error, AML_MODPACK_ERROR, AML_MODPACK_ERROR_FORMAT,
                           "the instance has no Minecraft version");
      return NULL;
    }

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "formatVersion");
  json_builder_add_int_value (builder, AML_MODPACK_FORMAT_VERSION);
  json_builder_set_member_name (builder, "game");
  json_builder_add_string_value (builder, "minecraft");
  json_builder_set_member_name (builder, "name");
  json_builder_add_string_value (builder,
                                 pack_name != NULL && *pack_name != '\0'
                                   ? pack_name
                                   : name != NULL ? name : "AML Instance");
  if (summary != NULL && *summary != '\0')
    {
      json_builder_set_member_name (builder, "summary");
      json_builder_add_string_value (builder, summary);
    }
  if (version_id != NULL && *version_id != '\0')
    {
      json_builder_set_member_name (builder, "versionId");
      json_builder_add_string_value (builder, version_id);
    }
  json_builder_set_member_name (builder, "dependencies");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "minecraft");
  json_builder_add_string_value (builder, version);
  if (loader != AML_LOADER_VANILLA && loader_version != NULL)
    {
      const char *loader_name = aml_loader_to_string (loader);

      if (loader == AML_LOADER_FABRIC)
        loader_name = "fabric-loader";
      else if (loader == AML_LOADER_QUILT)
        loader_name = "quilt-loader";

      json_builder_set_member_name (builder, loader_name);
      json_builder_add_string_value (builder, loader_version);
    }
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "files");
  json_builder_begin_array (builder);
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  result = json_generator_to_data (generator, &length);
  return g_steal_pointer (&result);
}

AmlTaskScope *
aml_modpack_export_async (AmlInstance *instance, const char *path,
                          const char *pack_name, const char *summary,
                          const char *version_id, gboolean include_transient,
                          AmlModpackExportProgressFunc progress,
                          gpointer progress_data,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = aml_task_scope_task (scope);
  AmlTaskScope *task_scope = scope;
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = NULL;
  ExportRun *run = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (path != NULL && g_path_is_absolute (path), NULL);

  run = g_new0 (ExportRun, 1);
  run->scope = task_scope;
  g_object_get (instance, "directory", &directory, NULL);
  run->instance_directory = g_strdup (directory);
  run->archive_path = g_strdup (path);
  run->include_transient = include_transient;
  run->progress = progress;
  run->progress_data = progress_data;
  run->indexed_paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              NULL);
  g_object_get (instance, "icon", &run->icon_path, NULL);
  if (run->icon_path != NULL &&
      !g_file_test (run->icon_path, G_FILE_TEST_IS_REGULAR))
    g_clear_pointer (&run->icon_path, g_free);
  run->manifest = export_manifest (instance, pack_name, summary, version_id,
                                   &error);

  if (run->instance_directory == NULL || run->manifest == NULL)
    {
      export_run_free (run);
      aml_task_scope_return_error_deferred (task_scope,
                                            g_steal_pointer (&error));
      return aml_task_scope_ref (task_scope);
    }

  g_task_set_task_data (task, run, (GDestroyNotify) export_run_free);
  g_task_run_in_thread (task, export_thread);
  return aml_task_scope_ref (task_scope);
}

gboolean
aml_modpack_export_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
