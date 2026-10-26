/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Resolve and download a Minecraft version.
 *
 * The version document supplies the JARs and asset index. Missing files are
 * downloaded and verified, native JARs are extracted, and virtual assets are
 * copied into place.
 *
 * Steps may start several transfers and advance after the last completes.
 * The first failure cancels the remaining work.
 *
 * Every transfer goes through nuvashalauncher_download_file_async(), which already
 * caps how many run at once, and every file this module reads or writes
 * of its own does so through GIO. Nothing here waits for a socket, for
 * a disk or for a jar in the main loop. */

#include "nuvashalauncher-minecraft-resolve-private.h"
#include "nuvashalauncher-task-private.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-fs.h"

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Required fields in a version document. */
#define NO_CLIENT_MESSAGE "this version document has no client jar"
#define NO_MAIN_CLASS_MESSAGE "this version document names no main class"

/* The objects of an asset index are named after their own hash and live
   in a directory named after its first characters, the way Mojang lays
   them out. */
#define ASSET_BUCKET_LENGTH 2

/* The directory the game reads a virtual asset index out of. */
#define ASSET_VIRTUAL_SUBDIR "virtual"

/* The prefix the resources of the game have in a map_to_resources index. */
#define ASSET_RESOURCE_PREFIX "minecraft/"

GQuark
nuvashalauncher_resolve_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-resolve-error");
}

/* ---------------------------------------------------------- *
 * The request that puts a whole version on disk
 * ---------------------------------------------------------- */

static void request_free (ResolveRequest *request);

void
request_fail (ResolveRequest *request, GError *error)
{
  if (error == NULL)
    return;

  if (request->error == NULL)
    request->error = error;
  else
    g_error_free (error);
}

/* A stage may discover a bad entry after it has already started other
 * operations.  Those callbacks still own a borrowed pointer to @request,
 * so cancellation alone is not enough: final cleanup waits for them. */
void
request_fail_and_wait (ResolveRequest *request, GError *error)
{
  request_fail (request, error);
  g_cancellable_cancel (request->cancellable);

  if (request->pending == 0)
    resolve_finish (request);
}

void
report (ResolveRequest *request)
{
  nuvashalauncherResolveProgress progress = { request->stage, request->done,
                                  request->total };

  if (request->progress != NULL)
    request->progress (&progress, request->progress_data);
}

/* Starts counting one more operation. */
void
operation_started (ResolveRequest *request)
{
  request->pending++;
}

/* One operation ended. When nothing is left the stage is over and
   whatever comes next runs. */
void
operation_finished (ResolveRequest *request, ResolveStep next, GError *error)
{
  request_fail (request, error);

  if (--request->pending == 0)
    {
      if (request->error != NULL)
        g_cancellable_cancel (request->cancellable);

      next (request);
    }
}

/* Complete the request with the resolved version or an error. */
static void
request_end (ResolveRequest *request, nuvashalauncherResolvedVersion *resolved)
{
  if (resolved != NULL)
    nuvashalauncher_task_scope_return_pointer (request->scope, resolved,
                                   (GDestroyNotify) nuvashalauncher_resolved_version_free);
  else if (request->error != NULL)
    nuvashalauncher_task_scope_return_error (request->scope,
                                 g_steal_pointer (&request->error));
  else
    nuvashalauncher_task_scope_return_error (
      request->scope,
      g_error_new_literal (nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_INVALID,
                           "the version cannot be resolved"));

  request_free (request);
}

static void
request_free (ResolveRequest *request)
{
  g_clear_object (&request->task);
  g_clear_object (&request->manifest);
  g_clear_object (&request->description);
  g_clear_pointer (&request->context, nuvashalauncher_rule_context_free);
  g_clear_object (&request->cancellable);
  g_clear_error (&request->error);
  g_clear_pointer (&request->natives, g_ptr_array_unref);
  g_clear_pointer (&request->asset_hashes, g_ptr_array_unref);
  g_clear_pointer (&request->asset_paths, g_hash_table_unref);
  g_free (request->version_id);
  g_free (request->data_dir);
  g_free (request->libraries_dir);
  g_free (request->versions_dir);
  g_free (request->assets_dir);
  g_free (request->indexes_dir);
  g_free (request->objects_dir);
  g_free (request->virtual_dir);
  g_free (request->natives_dir);
  g_free (request->client_jar);
  g_free (request->client_jar_override);
  g_free (request->logging_path);
  g_free (request->asset_index_id);
  g_free (request->asset_index_name);
  g_free (request->asset_base_url);
  g_free (request);
}

static void resolve_dependencies (ResolveRequest *request);
static void resolve_natives (ResolveRequest *request);

/* ---------------------------------------------------------- *
 * Downloads
 * ---------------------------------------------------------- */

static void
file_downloaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  PendingOperation *pending = user_data;
  ResolveRequest *request = pending->request;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_download_file_finish (result, &error))
    operation_finished (request, pending->next, g_steal_pointer (&error));
  else
    {
      request->done++;
      report (request);
      operation_finished (request, pending->next, NULL);
    }

  g_free (pending);
}

void
download_file (ResolveRequest *request, ResolveStep next, const char *url,
               const char *path, const char *sha1)
{
  PendingOperation *pending = g_new0 (PendingOperation, 1);

  pending->request = request;
  pending->next = next;

  operation_started (request);

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), url, path, sha1, G_CHECKSUM_SHA1, NULL,
                             file_downloaded, pending),
    request->cancellable);
}

/* ---------------------------------------------------------- *
 * The result
 * ---------------------------------------------------------- */

void
nuvashalauncher_resolved_version_free (nuvashalauncherResolvedVersion *resolved)
{
  if (resolved == NULL)
    return;

  g_free (resolved->version_id);
  g_free (resolved->version_type);
  g_free (resolved->client_jar);
  g_free (resolved->classpath);
  g_free (resolved->natives_directory);
  g_free (resolved->assets_directory);
  g_free (resolved->assets_index_name);
  g_free (resolved->main_class);
  g_free (resolved->library_directory);
  g_free (resolved->logging_argument);
  g_clear_pointer (&resolved->jvm_arguments, g_ptr_array_unref);
  g_clear_pointer (&resolved->game_arguments, g_ptr_array_unref);
  g_free (resolved);
}

void
resolve_finish (ResolveRequest *request)
{
  g_autoptr (GError) error = NULL;
  nuvashalauncherResolvedVersion *resolved = NULL;

  if (request->error == NULL)
    {
      resolved = build_result (request, &error);

      /* Propagate failures while assembling the result. */
      request_fail (request, g_steal_pointer (&error));
    }

  request_end (request, resolved);
}

/* ---------------------------------------------------------- *
 * Dependencies: the client jar, the libraries, the logging file
 * ---------------------------------------------------------- */

static void
natives_extracted (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  ResolveRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_resolve_extract_natives_finish (result, &error))
    request_fail (request, g_steal_pointer (&error));

  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  resolve_assets (request);
}

static void
resolve_dependencies (ResolveRequest *request)
{
  nuvashalauncherVersionDescription *description = request->description;
  g_autoptr (GPtrArray) files = NULL;
  g_autoptr (GError) error = NULL;

  if (description->client == NULL || description->client->url == NULL)
    {
      request_fail (request,
                    g_error_new (nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_INVALID,
                                 "%s", NO_CLIENT_MESSAGE));
      resolve_finish (request);
      return;
    }

  files = nuvashalauncher_resolve_library_files (description, request->context,
                                     request->libraries_dir);

  /* Unpack the native JARs collected during planning. */
  for (guint i = 0; i < files->len; i++)
    {
      const nuvashalauncherResolveFile *file = g_ptr_array_index (files, i);

      if (file->natives)
        g_ptr_array_add (request->natives,
                         nuvashalauncher_resolve_file_new (file->path, file->url,
                                               file->sha1, TRUE,
                                               file->exclude));
    }

  if (!nuvashalauncher_resolve_ensure_parent_directory (request->client_jar, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      resolve_finish (request);
      return;
    }

  request->stage = "dependencies";
  request->done = 0;
  /* The client jar, whether it is fetched from the document or handed in
     by a loader that patched one. */
  request->total = files->len + 1;

  if (description->logging != NULL && description->logging->file != NULL &&
      description->logging->file->url != NULL)
    {
      if (!nuvashalauncher_resolve_ensure_parent_directory (request->logging_path, &error))
        {
          request_fail (request, g_steal_pointer (&error));
          resolve_finish (request);
          return;
        }

      request->total++;
    }
  else
    {

      g_clear_pointer (&request->logging_path, g_free);
    }

  /* The download module writes into a directory that already exists. */
  for (guint i = 0; i < files->len; i++)
    {
      const nuvashalauncherResolveFile *file = g_ptr_array_index (files, i);

      if (!nuvashalauncher_resolve_ensure_parent_directory (file->path, &error))
        {
          request_fail (request, g_steal_pointer (&error));
          resolve_finish (request);
          return;
        }
    }

  for (guint i = 0; i < files->len; i++)
    {
      const nuvashalauncherResolveFile *file = g_ptr_array_index (files, i);

      download_file (request, resolve_natives, file->url, file->path,
                     file->sha1);
    }

  /* A loader that patches the client jar produced one already: it is run
     as it is, and nothing is fetched for it. Counting it as one of the
     files keeps the progress of a loader instance the same as it is
     without one. */
  if (request->client_jar_override != NULL)
    {
      request->done++;
      report (request);
    }
  else
    download_file (request, resolve_natives, description->client->url,
                   request->client_jar, description->client->sha1);

  if (request->logging_path != NULL)
    download_file (request, resolve_natives, description->logging->file->url,
                   request->logging_path, description->logging->file->sha1);

  report (request);
}

static void
resolve_natives (ResolveRequest *request)
{
  g_autoptr (GError) error = NULL;

  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  if (request->natives->len == 0)
    {
      resolve_assets (request);
      return;
    }

  if (!nuvashalauncher_resolve_ensure_directory (request->natives_dir, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      resolve_finish (request);
      return;
    }

  {
    nuvashalauncherTaskScope *extracting = nuvashalauncher_resolve_extract_natives_async (
      request->natives, request->natives_dir, natives_extracted, request);

    nuvashalauncher_task_scope_follow_cancellable (extracting, request->cancellable);
    nuvashalauncher_task_scope_unref_when_done (extracting);
  }
}

/* ---------------------------------------------------------- *
 * Assets
 * ---------------------------------------------------------- */

/* Where the object named @hash belongs, and the two characters that
   name the bucket it lives in. NULL when @hash is no usable name. */
static void
description_ready (ResolveRequest *request)
{
  /* The configuration log4j is given keeps the name the document gave
     it, or a plain one when it gave none. */
  if (request->description->logging != NULL &&
      request->description->logging->file != NULL)
    {
      g_autofree char *logging_name = nuvashalauncher_resolve_safe_name (
          request->description->logging->file->id != NULL
            ? request->description->logging->file->id
            : "client-logging.xml");

      request->logging_path = g_build_filename (request->data_dir, "logging",
                                                logging_name, NULL);
    }

  /* The game is told what the index is called, which the document
     answers in "assets", and the copy of it on disk is named after the
     id the index itself carries. */
  g_free (request->asset_index_name);
  request->asset_index_name = nuvashalauncher_resolve_safe_name (
    request->description->assets != NULL ? request->description->assets
                                         : request->version_id);

  g_free (request->asset_index_id);

  if (request->description->asset_index != NULL &&
      request->description->asset_index->id != NULL)
    {
      g_autofree char *safe =
        nuvashalauncher_resolve_safe_name (request->description->asset_index->id);

      request->asset_index_id = g_strconcat (safe, ".json", NULL);
    }
  else
    {
      g_autofree char *safe =
        nuvashalauncher_resolve_safe_name (request->description->assets != NULL
                     ? request->description->assets
                     : request->version_id);

      request->asset_index_id = g_strconcat (safe, ".json", NULL);
    }

  resolve_dependencies (request);
}

static void
version_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  ResolveRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  request->description = nuvashalauncher_version_load_finish (result, &error);

  if (request->description == NULL)
    {
      request_fail (request, g_steal_pointer (&error));
      resolve_finish (request);
      return;
    }

  description_ready (request);
}

/* Everything a resolve needs before it knows which document it resolves:
   where the files go, how it may be cancelled and who to tell about the
 * progress. The description is added by the caller. */
static ResolveRequest *
request_new (const char         *version_id,
             const char         *data_dir,
             const char         *natives_dir,
             const char         *asset_base_url,
             nuvashalauncherTaskScope       *scope,
             nuvashalauncherResolveProgressFunc progress,
             gpointer              progress_data)
{
  ResolveRequest *request = g_new0 (ResolveRequest, 1);
  g_autofree char *name = nuvashalauncher_resolve_safe_name (version_id);
  g_autofree char *jar = g_strconcat (name, ".jar", NULL);

  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->task = g_object_ref (nuvashalauncher_task_scope_task (scope));
  request->scope = scope;
  g_task_set_check_cancellable (request->task, FALSE);
  request->version_id = g_strdup (version_id);
  request->data_dir = data_dir != NULL
                        ? g_strdup (data_dir)
                        : g_build_filename (g_get_user_data_dir (),
                                            nuvashalauncher_RESOLVE_DATA_SUBDIR, NULL);
  request->libraries_dir =
    g_build_filename (request->data_dir, "libraries", NULL);
  request->assets_dir = g_build_filename (request->data_dir, "assets", NULL);
  request->indexes_dir =
    g_build_filename (request->assets_dir, "indexes", NULL);
  request->objects_dir =
    g_build_filename (request->assets_dir, "objects", NULL);
  request->virtual_dir =
    g_build_filename (request->assets_dir, ASSET_VIRTUAL_SUBDIR, NULL);
  request->natives_dir = natives_dir != NULL
                           ? g_strdup (natives_dir)
                           : g_build_filename (request->data_dir, "natives",
                                               NULL);
  request->asset_base_url = asset_base_url != NULL
                              ? g_strdup (asset_base_url)
                              : g_strdup (nuvashalauncher_RESOLVE_ASSET_BASE_URL);
  request->context = nuvashalauncher_rule_context_new ();
  request->progress = progress;
  request->progress_data = progress_data;
  request->cancellable = g_cancellable_new ();
  request->asset_hashes = g_ptr_array_new_with_free_func (g_free);
  request->asset_paths = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
  request->natives =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_resolve_file_free);
  request->versions_dir =
    g_build_filename (request->data_dir, "versions", name, NULL);
  request->client_jar = g_build_filename (request->versions_dir, jar, NULL);

  /* The operation owns the cancellation: whoever started it stops it
     through the handle it was given. The cancellable above is cancelled on
     a failure as well, which the caller knows nothing about. */
  nuvashalauncher_task_scope_follow_cancellable (scope, request->cancellable);

  return request;
}

/* The directories the resolve writes into have to be there before
   anything is fetched into them. */
static gboolean
request_make_directories (ResolveRequest *request)
{
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_resolve_ensure_directory (request->libraries_dir, &error) ||
      !nuvashalauncher_resolve_ensure_directory (request->assets_dir, &error) ||
      !nuvashalauncher_resolve_ensure_directory (request->versions_dir, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      return FALSE;
    }

  return TRUE;
}

nuvashalauncherTaskScope *
nuvashalauncher_resolve_version_async (nuvashalauncherVersionManifest *manifest,
                           const char         *version_id,
                           const char         *data_dir,
                           const char         *natives_dir,
                           const char         *asset_base_url,
                           nuvashalauncherResolveProgressFunc progress,
                           gpointer              progress_data,
                           GAsyncReadyCallback   callback,
                           gpointer              user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  ResolveRequest *request;

  g_return_val_if_fail (nuvashalauncher_IS_VERSION_MANIFEST (manifest), NULL);
  g_return_val_if_fail (version_id != NULL, NULL);

  request = request_new (version_id, data_dir, natives_dir, asset_base_url,
                         scope, progress, progress_data);
  request->manifest = g_object_ref (manifest);

  if (!request_make_directories (request))
    {
      resolve_finish (request);
      return g_steal_pointer (&scope);
    }

  {
    nuvashalauncherTaskScope *loading = nuvashalauncher_version_load_async (manifest, version_id,
                                                    version_loaded, request);

    nuvashalauncher_task_scope_follow_cancellable (loading, request->cancellable);
    nuvashalauncher_task_scope_unref_when_done (loading);
  }

  return g_steal_pointer (&scope);
}

nuvashalauncherTaskScope *
nuvashalauncher_resolve_description_async (nuvashalauncherVersionDescription *description,
                               const char             *version_id,
                               const char             *client_jar,
                               const char             *data_dir,
                               const char             *natives_dir,
                               const char             *asset_base_url,
                               nuvashalauncherResolveProgressFunc   progress,
                               gpointer                progress_data,
                               GAsyncReadyCallback     callback,
                               gpointer                user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  ResolveRequest *request;

  g_return_val_if_fail (nuvashalauncher_IS_VERSION_DESCRIPTION (description), NULL);
  g_return_val_if_fail (version_id != NULL, NULL);
  g_return_val_if_fail (client_jar == NULL || g_file_test (client_jar,
                                                           G_FILE_TEST_IS_REGULAR),
                        NULL);

  request = request_new (version_id, data_dir, natives_dir, asset_base_url,
                         scope, progress, progress_data);

  /* The document is the one the caller merged: there is no manifest to
     follow an inheritsFrom from, because it has been followed already. */
  request->description = g_object_ref (description);

  if (client_jar != NULL)
    request->client_jar_override = g_strdup (client_jar);

  if (!request_make_directories (request))
    {
      resolve_finish (request);
      return g_steal_pointer (&scope);
    }

  description_ready (request);

  return g_steal_pointer (&scope);
}

nuvashalauncherResolvedVersion *
nuvashalauncher_resolve_version_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
