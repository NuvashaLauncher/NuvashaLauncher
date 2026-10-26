/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Mojang's launcher metadata: parsing the version manifest, parsing a
 * version document, merging a profile with the version it inherits
 * from and judging the rules against this machine.
 *
 * Parsing only reads the bytes it is given and builds a tree of plain
 * structures; the two asynchronous entry points at the end of the file
 * fetch the documents through the download module, which verifies
 * every file against the sha1 the manifest publishes and keeps it in
 * g_get_user_cache_dir()/nuvashalauncher/. */

#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-task-private.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-fs.h"

#include <errno.h>
#include <string.h>
#include <sys/utsname.h>

/* The manifest itself, cached beside the version documents. */
#define MANIFEST_FILE "manifest.json"

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

/* g_get_user_cache_dir()/nuvashalauncher, created on the spot. */
static char *
ensure_cache_directory (GError **error)
{
  g_autofree char *directory =
    g_build_filename (g_get_user_cache_dir (), nuvashalauncher_VERSIONS_CACHE_SUBDIR, NULL);

  g_autoptr (GError) local_error = NULL;
  if (!nuvashalauncher_fs_make_directory (directory, 0755, &local_error))
    {
      g_set_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_IO,
                   "cannot create \"%s\": %s", directory,
                   local_error->message);
      return NULL;
    }

  return g_steal_pointer (&directory);
}

/* A version id is a file name only after everything that a file name
 * cannot hold has been taken out of it. */
static char *
safe_file_name (const char *id)
{
  char *name = g_strdup (id);

  for (char *p = name; *p != '\0'; p++)
    {
      if (!g_ascii_isalnum (*p) && *p != '.' && *p != '-' && *p != '_')
        *p = '_';
    }

  return name;
}

/* *path receives the cache file of @id and the directory behind it is
 * created. */
static gboolean
version_cache_path (const char *id, char **path, GError **error)
{
  g_autofree char *directory = ensure_cache_directory (error);
  g_autofree char *name = NULL;
  g_autofree char *file = NULL;

  if (directory == NULL)
    return FALSE;

  name = safe_file_name (id);
  file = g_strconcat (name, ".json", NULL);
  *path = g_build_filename (directory, file, NULL);

  return TRUE;
}

typedef struct
{
  nuvashalauncherTaskSequence *sequence;
  char *url;
  char *path;
} FetchRequest;

typedef struct
{
  nuvashalauncherTaskSequence *sequence;
  char *path;       /* cache file of the document being read */
  char *next_id;    /* the version to fetch next, NULL when there is none */
  nuvashalauncherVersionManifest *manifest;
  nuvashalauncherVersionDescription *merged;
  GHashTable *seen;              /* ids already on this chain */
} LoadRequest;

static void
fetch_request_free (FetchRequest *request)
{
  g_free (request->path);
  g_free (request->url);
  g_free (request);
}

static void
load_request_free (LoadRequest *request)
{
  g_free (request->path);
  g_free (request->next_id);
  g_clear_object (&request->manifest);
  g_clear_object (&request->merged);
  g_clear_pointer (&request->seen, g_hash_table_unref);
  g_free (request);
}

static void load_step (LoadRequest *request);

static void
load_sequence_step (nuvashalauncherTaskSequence *sequence G_GNUC_UNUSED,
                    gpointer user_data)
{
  load_step (user_data);
}

static void
manifest_file_loaded (GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
  FetchRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;
  nuvashalauncherVersionManifest *manifest;

  if (!g_file_load_contents_finish (G_FILE (source), result, &contents,
                                    &length, NULL, &error))
    {
      nuvashalauncher_task_sequence_return_error (
        request->sequence,
        g_error_new (nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_IO,
                     "the downloaded manifest cannot be read: %s",
                     error->message));
      return;
    }

  if (length > nuvashalauncher_FS_MAX_JSON_DOCUMENT_SIZE)
    {
      nuvashalauncher_task_sequence_return_error (
        request->sequence,
        g_error_new_literal (nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_IO,
                             "the downloaded manifest is too large"));
      return;
    }

  manifest = nuvashalauncher_version_manifest_new_from_json (contents, (gssize) length,
                                                 &error);

  if (manifest == NULL)
    {
      nuvashalauncher_task_sequence_return_error (request->sequence,
                                      g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_task_sequence_return_pointer (request->sequence, manifest,
                                    g_object_unref);
}

static void
manifest_downloaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  FetchRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = g_file_new_for_path (request->path);

  (void) source;

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      nuvashalauncher_task_sequence_return_error (request->sequence,
                                      g_steal_pointer (&error));
      return;
    }

  g_file_load_contents_async (
    file,
    nuvashalauncher_task_sequence_get_cancellable (request->sequence),
                              manifest_file_loaded, request);
}

static void
manifest_fetch_step (nuvashalauncherTaskSequence *sequence G_GNUC_UNUSED,
                     gpointer user_data)
{
  FetchRequest *request = user_data;

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), request->url, request->path, NULL,
                             G_CHECKSUM_SHA1, NULL, manifest_downloaded,
                             request),
    nuvashalauncher_task_sequence_get_cancellable (request->sequence));
}

nuvashalauncherTaskScope *
nuvashalauncher_version_manifest_fetch_async (const char         *url,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  FetchRequest *request;
  nuvashalauncherTaskScope *scope = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = NULL;
  g_return_val_if_fail (url != NULL, NULL);

  directory = ensure_cache_directory (&error);

  if (directory == NULL)
    {
      /* The task reports back through an idle, so the caller never
       * hears from us before this function has returned. */
      g_autoptr (nuvashalauncherTaskScope) failure =
        nuvashalauncher_task_scope_new (NULL, cancellable, callback, user_data);

      nuvashalauncher_task_scope_set_check_cancellable (failure, FALSE);
      nuvashalauncher_task_scope_return_error_deferred (failure,
                                            g_steal_pointer (&error));
      return g_steal_pointer (&failure);
    }

  request = g_new0 (FetchRequest, 1);
  request->url = g_strdup (url);
  request->path = g_build_filename (directory, MANIFEST_FILE, NULL);
  request->sequence =
    nuvashalauncher_task_sequence_new (cancellable, callback, user_data,
                           manifest_fetch_step, request,
                           (GDestroyNotify) fetch_request_free);

  /* Kept on the stack, not read back out of the request: a sequence that
     answers at once frees the request, and this is past that. */
  {
    nuvashalauncherTaskSequence *sequence = request->sequence;

    /* The handle the caller gets back stands for the whole run. The
       sequence keeps the task itself. */
    scope = nuvashalauncher_task_sequence_scope (sequence);

    nuvashalauncher_task_sequence_start (sequence);
    nuvashalauncher_task_sequence_unref (sequence);
  }

  return scope;
}

nuvashalauncherVersionManifest *
nuvashalauncher_version_manifest_fetch_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
load_file_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;
  nuvashalauncherVersionDescription *document;

  if (!g_file_load_contents_finish (G_FILE (source), result, &contents,
                                    &length, NULL, &error))
    {
      nuvashalauncher_task_sequence_return_error (
        request->sequence,
        g_error_new (nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_IO,
                     "the downloaded version cannot be read: %s",
                     error->message));
      return;
    }

  if (length > nuvashalauncher_FS_MAX_JSON_DOCUMENT_SIZE)
    {
      nuvashalauncher_task_sequence_return_error (
        request->sequence,
        g_error_new_literal (nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_IO,
                             "the downloaded version document is too large"));
      return;
    }

  document = nuvashalauncher_version_description_new_from_json (contents, (gssize) length,
                                                    &error);

  if (document == NULL)
    {
      nuvashalauncher_task_sequence_return_error (request->sequence,
                                      g_steal_pointer (&error));
      return;
    }

  if (request->merged == NULL)
    request->merged = document;
  else
    {
      /* The document just read is the parent, the accumulator the
       * child built out of everything below it. */
      nuvashalauncherVersionDescription *combined =
        nuvashalauncher_version_description_merge (document, request->merged);

      g_object_unref (document);
      g_object_unref (request->merged);
      request->merged = combined;
    }

  if (request->merged->inherits_from == NULL)
    {
      nuvashalauncherVersionDescription *merged_result =
        g_steal_pointer (&request->merged);

      nuvashalauncher_task_sequence_return_pointer (request->sequence, merged_result,
                                        g_object_unref);
      return;
    }

  g_clear_pointer (&request->next_id, g_free);
  g_clear_pointer (&request->path, g_free);
  request->next_id = g_strdup (request->merged->inherits_from);

  nuvashalauncher_task_sequence_next (request->sequence);
}

static void
load_downloaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = g_file_new_for_path (request->path);

  (void) source;

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      nuvashalauncher_task_sequence_return_error (request->sequence,
                                      g_steal_pointer (&error));
      return;
    }

  g_file_load_contents_async (
    file,
    nuvashalauncher_task_sequence_get_cancellable (request->sequence),
                              load_file_loaded, request);
}

/* Starts the download of the version @request->next_id points at, or
 * ends the request when that version cannot be reached. */
static void
load_step (LoadRequest *request)
{
  const nuvashalauncherVersionEntry *entry;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;

  entry = nuvashalauncher_version_manifest_find (request->manifest, request->next_id);

  if (entry == NULL)
    {
      nuvashalauncher_task_sequence_return_error (
        request->sequence,
        g_error_new (nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_NOT_FOUND,
                     "the manifest knows no version \"%s\"",
                     request->next_id));
      return;
    }

  if (g_hash_table_contains (request->seen, request->next_id))
    {
      nuvashalauncher_task_sequence_return_error (
        request->sequence,
        g_error_new (nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_CYCLE,
                     "\"%s\" inherits from a version that inherits from it",
                     request->next_id));
      return;
    }

  g_hash_table_insert (request->seen, g_strdup (request->next_id),
                       GINT_TO_POINTER (1));

  if (!version_cache_path (request->next_id, &path, &error))
    {
      nuvashalauncher_task_sequence_return_error (request->sequence,
                                      g_steal_pointer (&error));
      return;
    }

  request->path = g_steal_pointer (&path);

  /* The manifest's sha1 decides whether the copy on disk is good
   * enough, so a damaged one is downloaded again. */
  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), entry->url, request->path, entry->sha1,
                             G_CHECKSUM_SHA1, NULL, load_downloaded, request),
    nuvashalauncher_task_sequence_get_cancellable (request->sequence));
}

nuvashalauncherTaskScope *
nuvashalauncher_version_load_async (nuvashalauncherVersionManifest *manifest,
                        const char         *version_id,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  nuvashalauncherTaskScope *scope = NULL;
  LoadRequest *request;

  g_return_val_if_fail (nuvashalauncher_IS_VERSION_MANIFEST (manifest), NULL);
  g_return_val_if_fail (version_id != NULL, NULL);

  request = g_new0 (LoadRequest, 1);
  request->manifest = g_object_ref (manifest);
  request->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  request->next_id = g_strdup (version_id);

  request->sequence =
    nuvashalauncher_task_sequence_new (cancellable, callback, user_data,
                           load_sequence_step, request,
                           (GDestroyNotify) load_request_free);

  /* Kept on the stack, not read back out of the request: a sequence that
     answers at once frees the request, and this is past that. */
  {
    nuvashalauncherTaskSequence *sequence = request->sequence;

    /* The handle the caller gets back stands for the whole run. The
       sequence keeps the task itself. */
    scope = nuvashalauncher_task_sequence_scope (sequence);

    nuvashalauncher_task_sequence_start (sequence);
    nuvashalauncher_task_sequence_unref (sequence);
  }

  return scope;
}

nuvashalauncherVersionDescription *
nuvashalauncher_version_load_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
