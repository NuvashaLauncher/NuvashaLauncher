/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlResolve: putting a version onto this disk.
 *
 * See aml-minecraft-resolve.h for what the module is for. The work is a
 * chain: the version document says which jars and which asset index are
 * needed, the download module fetches what is missing and verifies it
 * against the sha1 the document published, the natives jars are unpacked
 * with libarchive and the asset objects are copied into place when the
 * index asks for a virtual directory.
 *
 * Every step hands the next one the baton. A step that starts several
 * transfers at once counts them, so the step after it begins when the
 * last one reports back; the first failure is remembered and told to
 * the rest through a cancellable, which keeps the queue inside the
 * download module from running on behind a request nobody waits for.
 *
 * Every transfer goes through aml_download_file_async(), which already
 * caps how many run at once, and every file this module reads or writes
 * of its own does so through GIO. Nothing here waits for a socket, for
 * a disk or for a jar in the main loop. */

#include "aml-minecraft-resolve-private.h"
#include "aml-task-private.h"

#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-fs.h"

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* What a version document has to carry before AML can do anything with
   it. */
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

/* What -cp wants between the jars. */
#define CLASSPATH_SEPARATOR ":"

GQuark
aml_resolve_error_quark (void)
{
  return g_quark_from_static_string ("aml-resolve-error");
}

/* ---------------------------------------------------------- *
 * The request that puts a whole version on disk
 * ---------------------------------------------------------- */

typedef struct ResolveRequest ResolveRequest;

typedef void (*ResolveStep) (ResolveRequest *request);

/* One transfer in flight. */
typedef struct
{
  ResolveRequest *request;
  ResolveStep next; /* what runs once nothing is in flight any more */
} PendingOperation;

struct ResolveRequest
{
  GTask *task;
  AmlTaskScope *scope;
  AmlVersionManifest *manifest;
  AmlVersionDescription *description;

  char *version_id;
  char *data_dir;
  char *libraries_dir;
  char *versions_dir;
  char *assets_dir;
  char *indexes_dir;
  char *objects_dir;
  char *virtual_dir;
  char *natives_dir;
  char *client_jar;
  /* The jar a loader patched instead of the one the document names: a
     file that is already there, so nothing is fetched for it. */
  char *client_jar_override;
  char *logging_path;
  char *asset_index_id;   /* the file name of the index, "<id>.json" */
  char *asset_index_name; /* what the game calls it */
  char *asset_base_url;

  AmlRuleContext *context;
  AmlResolveProgressFunc progress;
  gpointer progress_data;

  /* The first failure is kept; the rest of the queue is told to stop
     through this cancellable, which the operation's own cancellation
     follows. */
  GCancellable *cancellable;
  GError *error;

  const char *stage; /* what the progress refers to */
  guint pending;     /* operations in flight */
  guint done;        /* files finished in the current stage */
  guint total;       /* files in the current stage */

  /* The jars to unpack, filled while the libraries are planned. */
  GPtrArray *natives;

  /* What the asset index asked for. */
  GPtrArray *asset_hashes; /* char* */
  GHashTable *asset_paths; /* hash -> char* */
  gboolean asset_virtual;
  gboolean asset_map_to_resources;
};

static void request_free (ResolveRequest *request);
static void resolve_finish (ResolveRequest *request);

static void
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
static void
request_fail_and_wait (ResolveRequest *request, GError *error)
{
  request_fail (request, error);
  g_cancellable_cancel (request->cancellable);

  if (request->pending == 0)
    resolve_finish (request);
}

static void
report (ResolveRequest *request)
{
  AmlResolveProgress progress = { request->stage, request->done,
                                  request->total };

  if (request->progress != NULL)
    request->progress (&progress, request->progress_data);
}

/* Starts counting one more operation. */
static void
operation_started (ResolveRequest *request)
{
  request->pending++;
}

/* One operation ended. When nothing is left the stage is over and
   whatever comes next runs. */
static void
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

/* Gives the caller what was prepared, or the failure, once and once
   only. */
static void
request_end (ResolveRequest *request, AmlResolvedVersion *resolved)
{
  if (resolved != NULL)
    aml_task_scope_return_pointer (request->scope, resolved,
                                   (GDestroyNotify) aml_resolved_version_free);
  else if (request->error != NULL)
    aml_task_scope_return_error (request->scope,
                                 g_steal_pointer (&request->error));
  else
    aml_task_scope_return_error (
      request->scope,
      g_error_new_literal (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_INVALID,
                           "the version cannot be resolved"));

  request_free (request);
}

static void
request_free (ResolveRequest *request)
{
  g_clear_object (&request->task);
  g_clear_object (&request->manifest);
  g_clear_object (&request->description);
  g_clear_pointer (&request->context, aml_rule_context_free);
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
static void resolve_assets (ResolveRequest *request);
static void resolve_copies (ResolveRequest *request);

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

  if (!aml_download_file_finish (result, &error))
    operation_finished (request, pending->next, g_steal_pointer (&error));
  else
    {
      request->done++;
      report (request);
      operation_finished (request, pending->next, NULL);
    }

  g_free (pending);
}

static void
download_file (ResolveRequest *request, ResolveStep next, const char *url,
               const char *path, const char *sha1)
{
  PendingOperation *pending = g_new0 (PendingOperation, 1);

  pending->request = request;
  pending->next = next;

  operation_started (request);

  aml_task_scope_release_when_done (
    aml_download_file_async (aml_downloads_get_service (), url, path, sha1, G_CHECKSUM_SHA1, NULL,
                             file_downloaded, pending),
    request->cancellable);
}

/* ---------------------------------------------------------- *
 * The result
 * ---------------------------------------------------------- */

void
aml_resolved_version_free (AmlResolvedVersion *resolved)
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

/* Replaces the ${...} placeholders whose value is known here. Anything
   else, which depends on an account or on a window, is left as the
   document wrote it. */
static char *
expand_argument (const char *value, GHashTable *known)
{
  GString *out = g_string_new (NULL);
  const char *p = value;

  while (*p != '\0')
    {
      const char *start = NULL;
      const char *end = NULL;
      const char *replacement = NULL;

      if (p[0] == '$' && p[1] == '{')
        {
          start = p + 2;
          end = strchr (start, '}');

          if (end != NULL)
            {
              /* The name is what is between the braces, and nothing
                 else: the rest of the argument follows the brace. */
              g_autofree char *name = g_strndup (start, end - start);

              replacement = g_hash_table_lookup (known, name);
            }
        }

      if (replacement == NULL)
        {
          g_string_append_c (out, *p);
          p++;
          continue;
        }

      g_string_append (out, replacement);
      p = end + 1;
    }

  return g_string_free (out, FALSE);
}

static GPtrArray *
expand_arguments (GPtrArray *arguments, GHashTable *known)
{
  g_autoptr (GPtrArray) expanded =
    g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < arguments->len; i++)
    g_ptr_array_add (
        expanded, expand_argument (g_ptr_array_index (arguments, i), known));

  return g_steal_pointer (&expanded);
}

/* Builds the answer out of what is on disk now. NULL on failure, with
   @error filled in. */
static AmlResolvedVersion *
build_result (ResolveRequest *request, GError **error)
{
  AmlResolvedVersion *resolved = g_new0 (AmlResolvedVersion, 1);
  g_autoptr (GPtrArray) jvm = NULL;
  g_autoptr (GPtrArray) game = NULL;
  g_autoptr (GHashTable) known = g_hash_table_new (g_str_hash, g_str_equal);

  /* The request reaches this point only after it holds a description. */
  if (request->description->main_class == NULL)
    {
      g_set_error (error, AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_INVALID,
                   "%s", NO_MAIN_CLASS_MESSAGE);
      g_free (resolved);
      return NULL;
    }

  /* A loader that patches the client jar produced one already: it is the
     jar the game runs from, and the one at the end of the class path,
     which is where the class loader finds the classes of the game. */
  {
    const char *client_jar = request->client_jar_override != NULL
                               ? request->client_jar_override
                               : request->client_jar;

    resolved->client_jar = g_strdup (client_jar);
    resolved->classpath =
      aml_resolve_classpath (request->description, request->context,
                             request->libraries_dir, client_jar);
  }

  resolved->version_id = g_strdup (request->description->id);
  resolved->version_type = g_strdup (
    aml_mc_version_type_to_string (request->description->type));
  resolved->natives_directory = g_strdup (request->natives_dir);
  resolved->library_directory = g_strdup (request->libraries_dir);
  resolved->main_class = g_strdup (request->description->main_class);

  if (resolved->classpath == NULL)
    {
      aml_resolved_version_free (resolved);
      return NULL;
    }

  if (request->description->java_version != NULL)
    resolved->java_major_version =
      request->description->java_version->major_version;

  /* A virtual index is a directory the game reads its assets from
     directly, so that directory is what it is given as its assets root
     and it is told no index name at all. */
  if (request->asset_virtual)
    {
      resolved->assets_directory =
        g_build_filename (request->virtual_dir, request->asset_index_name,
                          NULL);
      resolved->assets_index_name = NULL;
    }
  else
    {
      resolved->assets_directory = g_strdup (request->assets_dir);
      resolved->assets_index_name = g_strdup (request->asset_index_name);
    }

  if (request->description->logging != NULL &&
      request->logging_path != NULL)
    {
      g_autoptr (GHashTable) path_only =
        g_hash_table_new (g_str_hash, g_str_equal);

      g_hash_table_insert (path_only, "path", request->logging_path);
      resolved->logging_argument =
        expand_argument (request->description->logging->argument, path_only);
    }

  g_hash_table_insert (known, "classpath", resolved->classpath);
  g_hash_table_insert (known, "natives_directory", request->natives_dir);
  g_hash_table_insert (known, "library_directory", request->libraries_dir);
  g_hash_table_insert (known, "assets_root", resolved->assets_directory);
  /* Pre-1.7 version documents call the same directory game_assets. */
  g_hash_table_insert (known, "game_assets", resolved->assets_directory);
  g_hash_table_insert (known, "assets_index_name",
                       resolved->assets_index_name != NULL
                         ? resolved->assets_index_name
                         : request->asset_index_name);
  g_hash_table_insert (known, "version_name", resolved->version_id);
  g_hash_table_insert (known, "version_type", resolved->version_type);

  jvm = aml_resolve_jvm_arguments (request->description, request->context);
  game = aml_resolve_game_arguments (request->description, request->context);

  resolved->jvm_arguments = expand_arguments (jvm, known);
  resolved->game_arguments = expand_arguments (game, known);

  return resolved;
}

static void
resolve_finish (ResolveRequest *request)
{
  g_autoptr (GError) error = NULL;
  AmlResolvedVersion *resolved = NULL;

  if (request->error == NULL)
    {
      resolved = build_result (request, &error);

      /* What went wrong while putting the answer together is the
         failure of the request, not something to swallow. */
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

  if (!aml_resolve_extract_natives_finish (result, &error))
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
  AmlVersionDescription *description = request->description;
  g_autoptr (GPtrArray) files = NULL;
  g_autoptr (GError) error = NULL;

  if (description->client == NULL || description->client->url == NULL)
    {
      request_fail (request,
                    g_error_new (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_INVALID,
                                 "%s", NO_CLIENT_MESSAGE));
      resolve_finish (request);
      return;
    }

  files = aml_resolve_library_files (description, request->context,
                                     request->libraries_dir);

  /* The natives are remembered from the same walk, so the jars to
     unpack are exactly the ones that were fetched. */
  for (guint i = 0; i < files->len; i++)
    {
      const AmlResolveFile *file = g_ptr_array_index (files, i);

      if (file->natives)
        g_ptr_array_add (request->natives,
                         aml_resolve_file_new (file->path, file->url,
                                               file->sha1, TRUE,
                                               file->exclude));
    }

  if (!aml_resolve_ensure_parent_directory (request->client_jar, &error))
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
      if (!aml_resolve_ensure_parent_directory (request->logging_path, &error))
        {
          request_fail (request, g_steal_pointer (&error));
          resolve_finish (request);
          return;
        }

      request->total++;
    }
  else
    {
      /* Nothing to hand log4j later. */
      g_clear_pointer (&request->logging_path, g_free);
    }

  /* The download module writes into a directory that already exists. */
  for (guint i = 0; i < files->len; i++)
    {
      const AmlResolveFile *file = g_ptr_array_index (files, i);

      if (!aml_resolve_ensure_parent_directory (file->path, &error))
        {
          request_fail (request, g_steal_pointer (&error));
          resolve_finish (request);
          return;
        }
    }

  for (guint i = 0; i < files->len; i++)
    {
      const AmlResolveFile *file = g_ptr_array_index (files, i);

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

  if (!aml_resolve_ensure_directory (request->natives_dir, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      resolve_finish (request);
      return;
    }

  {
    AmlTaskScope *extracting = aml_resolve_extract_natives_async (
      request->natives, request->natives_dir, natives_extracted, request);

    aml_task_scope_follow_cancellable (extracting, request->cancellable);
    aml_task_scope_unref_when_done (extracting);
  }
}

/* ---------------------------------------------------------- *
 * Assets
 * ---------------------------------------------------------- */

/* Where the object named @hash belongs, and the two characters that
   name the bucket it lives in. NULL when @hash is no usable name. */
static char *
asset_object_path (ResolveRequest *request, const char *hash, char **bucket)
{
  g_autofree char *name = NULL;
  g_autofree char *directory = NULL;

  if (hash == NULL || strlen (hash) < ASSET_BUCKET_LENGTH)
    return NULL;

  name = g_strndup (hash, ASSET_BUCKET_LENGTH);
  directory = g_build_filename (request->objects_dir, name, NULL);

  if (bucket != NULL)
    *bucket = g_strdup (name);

  return g_build_filename (directory, hash, NULL);
}

/* One object was copied into the virtual directory. */
static void
asset_copied (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
              gpointer user_data)
{
  ResolveRequest *request = user_data;
  g_autoptr (GError) error = NULL;

  if (!aml_fs_copy_finish (G_FILE (source), result, &error))
    {
      operation_finished (request, resolve_finish, g_steal_pointer (&error));
      return;
    }

  request->done++;
  report (request);
  operation_finished (request, resolve_finish, NULL);
}

/* Copies the objects the index expects to find under the virtual
   directory of the assets. A copy that is already there is left alone,
   because a second resolve must not rewrite what the first one wrote. */
static void
resolve_copies (ResolveRequest *request)
{
  guint started = 0;

  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  for (guint i = 0; i < request->asset_hashes->len; i++)
    {
      const char *hash = g_ptr_array_index (request->asset_hashes, i);
      const char *path = g_hash_table_lookup (request->asset_paths, hash);
      g_autofree char *source = NULL;
      g_autofree char *target = NULL;
      g_autoptr (GFile) source_file = NULL;
      g_autoptr (GFile) target_file = NULL;
      g_autoptr (GError) error = NULL;

      if (path == NULL)
        continue;

      source = asset_object_path (request, hash, NULL);

      if (source == NULL)
        continue;

      target = g_build_filename (request->virtual_dir,
                                 request->asset_index_name, path, NULL);
      target_file = g_file_new_for_path (target);

      if (g_file_query_exists (target_file, NULL))
        continue;

      if (!aml_resolve_ensure_parent_directory (target, &error))
        {
          request_fail_and_wait (request, g_steal_pointer (&error));
          return;
        }

      source_file = g_file_new_for_path (source);

      operation_started (request);
      request->total++;
      started++;

      aml_fs_copy_async (source_file, target_file, G_FILE_COPY_OVERWRITE,
                         G_PRIORITY_DEFAULT, request->cancellable, NULL, NULL,
                         asset_copied, request);
    }

  report (request);

  if (started == 0)
    resolve_finish (request);
}

/* Every object of the index is on the disk now. */
static void
assets_downloaded (ResolveRequest *request)
{
  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  /* A virtual index is read as files, so the game is pointed at the
     copies; every other index is read from the objects themselves. */
  if (request->asset_virtual)
    resolve_copies (request);
  else
    resolve_finish (request);
}

static void
objects_downloaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  PendingOperation *pending = user_data;
  ResolveRequest *request = pending->request;
  g_autoptr (GError) error = NULL;

  if (!aml_download_file_finish (result, &error))
    operation_finished (request, assets_downloaded, g_steal_pointer (&error));
  else
    {
      request->done++;
      report (request);
      operation_finished (request, assets_downloaded, NULL);
    }

  g_free (pending);
}

/* The index is on the disk: every object it names is fetched. The whole
   list is handed to the download module at once, which is what caps how
   many of them run at a time. */
static void
resolve_objects (ResolveRequest *request)
{
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autofree char *text = NULL;
  gsize length = 0;
  JsonNode *root_node = NULL;
  JsonObject *root = NULL;
  JsonObject *objects = NULL;
  JsonObjectIter iter;
  const char *name = NULL;
  JsonNode *node = NULL;
  guint started = 0;

  if (request->error != NULL)
    {
      resolve_finish (request);
      return;
    }

  path = g_build_filename (request->indexes_dir, request->asset_index_id, NULL);

  if (!aml_fs_get_contents_within (path, AML_FS_MAX_ASSET_INDEX_SIZE,
                                   &text, &length, &error) ||
      !json_parser_load_from_data (parser, text, (gssize) length, &error) ||
      !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      g_autoptr (GError) local = g_steal_pointer (&error);

      request_fail (request,
                    g_error_new (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_ASSETS,
                                 "the asset index \"%s\" cannot be read: %s",
                                 path,
                                 local != NULL ? local->message
                                               : "it is not a JSON object"));
      resolve_finish (request);
      return;
    }

  root_node = json_parser_get_root (parser);
  root = json_node_get_object (root_node);

  if (!json_object_has_member (root, "objects") ||
      !JSON_NODE_HOLDS_OBJECT (json_object_get_member (root, "objects")))
    {
      request_fail (request,
                    g_error_new (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_ASSETS,
                                 "the asset index \"%s\" has no objects",
                                 path));
      resolve_finish (request);
      return;
    }

  objects = json_object_get_object_member (root, "objects");
  request->asset_virtual = json_object_has_member (root, "virtual") &&
                           json_object_get_boolean_member (root, "virtual");
  request->asset_map_to_resources =
    json_object_has_member (root, "map_to_resources") &&
    json_object_get_boolean_member (root, "map_to_resources");

  /* The index is one of the files of this stage, and it is done. */
  request->total = request->done + json_object_get_size (objects);
  report (request);

  json_object_iter_init (&iter, objects);

  while (json_object_iter_next (&iter, &name, &node))
    {
      const char *hash = name;
      const char *asset_path = NULL;
      g_autofree char *destination = NULL;
      g_autofree char *bucket = NULL;
      g_autofree char *url = NULL;
      g_autoptr (GError) local = NULL;
      PendingOperation *pending;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        {
          request_fail_and_wait (
              request,
              g_error_new (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_ASSETS,
                           "the asset \"%s\" of \"%s\" is not an object",
                           name, path));
          return;
        }

      {
        JsonObject *object = json_node_get_object (node);

        if (json_object_has_member (object, "hash"))
          hash = json_object_get_string_member (object, "hash");

        if (json_object_has_member (object, "path"))
          asset_path = json_object_get_string_member (object, "path");
      }

      destination = asset_object_path (request, hash, &bucket);

      if (destination == NULL)
        {
          request_fail_and_wait (request,
                                 g_error_new (AML_RESOLVE_ERROR,
                                              AML_RESOLVE_ERROR_ASSETS,
                                              "\"%s\" is not a usable asset name",
                                              hash));
          return;
        }

      if (asset_path != NULL && !aml_resolve_entry_name_is_safe (asset_path))
        {
          request_fail_and_wait (
              request,
              g_error_new (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_UNSAFE_PATH,
                           "the asset \"%s\" of \"%s\" would be copied to "
                           "\"%s\", outside the assets directory",
                           hash, path, asset_path));
          return;
        }

      if (!aml_resolve_ensure_parent_directory (destination, &local))
        {
          request_fail_and_wait (request, g_steal_pointer (&local));
          return;
        }

      g_ptr_array_add (request->asset_hashes, g_strdup (hash));

      if (asset_path != NULL)
        {
          /* A map_to_resources index already carries the prefix the
             game looks under; one that does not is read as a resource
             of the game. */
          if (request->asset_virtual && request->asset_map_to_resources &&
              !g_str_has_prefix (asset_path, ASSET_RESOURCE_PREFIX))
            g_hash_table_insert (
                request->asset_paths, g_strdup (hash),
                g_strconcat (ASSET_RESOURCE_PREFIX, asset_path, NULL));
          else
            g_hash_table_insert (request->asset_paths, g_strdup (hash),
                                 g_strdup (asset_path));
        }

      url = g_strdup_printf ("%s/%s/%s", request->asset_base_url, bucket, hash);

      pending = g_new0 (PendingOperation, 1);
      pending->request = request;
      pending->next = assets_downloaded;

      operation_started (request);
      started++;

      /* An object is named after its own hash, so the bytes can be
         checked against it. */
      aml_task_scope_release_when_done (
        aml_download_file_async (aml_downloads_get_service (), url, destination, hash, G_CHECKSUM_SHA1,
                                 NULL, objects_downloaded, pending),
        request->cancellable);
    }

  if (started == 0)
    assets_downloaded (request);
}

static void
resolve_assets (ResolveRequest *request)
{
  AmlVersionDescription *description = request->description;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;

  if (description->asset_index == NULL ||
      description->asset_index->url == NULL)
    {
      /* A version this old needs no assets at all. */
      resolve_finish (request);
      return;
    }

  path = g_build_filename (request->indexes_dir, request->asset_index_id, NULL);

  if (!aml_resolve_ensure_parent_directory (path, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      resolve_finish (request);
      return;
    }

  request->stage = "assets";
  request->done = 0;
  request->total = 1;

  download_file (request, resolve_objects, description->asset_index->url, path,
                 description->asset_index->sha1);
  report (request);
}

/* ---------------------------------------------------------- *
 * Entry point
 * ---------------------------------------------------------- */

/* What the resolve needs to know about the document, once there is one:
   where its logging file goes and what its asset index is called. Both
   are asked of the document alone, whether it came from the manifest or
   from a loader profile merged over it. */
static void
description_ready (ResolveRequest *request)
{
  /* The configuration log4j is given keeps the name the document gave
     it, or a plain one when it gave none. */
  if (request->description->logging != NULL &&
      request->description->logging->file != NULL)
    {
      g_autofree char *logging_name = aml_resolve_safe_name (
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
  request->asset_index_name = aml_resolve_safe_name (
    request->description->assets != NULL ? request->description->assets
                                         : request->version_id);

  g_free (request->asset_index_id);

  if (request->description->asset_index != NULL &&
      request->description->asset_index->id != NULL)
    {
      g_autofree char *safe =
        aml_resolve_safe_name (request->description->asset_index->id);

      request->asset_index_id = g_strconcat (safe, ".json", NULL);
    }
  else
    {
      g_autofree char *safe =
        aml_resolve_safe_name (request->description->assets != NULL
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

  request->description = aml_version_load_finish (result, &error);

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
             AmlTaskScope       *scope,
             AmlResolveProgressFunc progress,
             gpointer              progress_data)
{
  ResolveRequest *request = g_new0 (ResolveRequest, 1);
  g_autofree char *name = aml_resolve_safe_name (version_id);
  g_autofree char *jar = g_strconcat (name, ".jar", NULL);

  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->task = g_object_ref (aml_task_scope_task (scope));
  request->scope = scope;
  g_task_set_check_cancellable (request->task, FALSE);
  request->version_id = g_strdup (version_id);
  request->data_dir = data_dir != NULL
                        ? g_strdup (data_dir)
                        : g_build_filename (g_get_user_data_dir (),
                                            AML_RESOLVE_DATA_SUBDIR, NULL);
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
                              : g_strdup (AML_RESOLVE_ASSET_BASE_URL);
  request->context = aml_rule_context_new ();
  request->progress = progress;
  request->progress_data = progress_data;
  request->cancellable = g_cancellable_new ();
  request->asset_hashes = g_ptr_array_new_with_free_func (g_free);
  request->asset_paths = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
  request->natives =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_resolve_file_free);
  request->versions_dir =
    g_build_filename (request->data_dir, "versions", name, NULL);
  request->client_jar = g_build_filename (request->versions_dir, jar, NULL);

  /* The operation owns the cancellation: whoever started it stops it
     through the handle it was given. The cancellable above is cancelled on
     a failure as well, which the caller knows nothing about. */
  aml_task_scope_follow_cancellable (scope, request->cancellable);

  return request;
}

/* The directories the resolve writes into have to be there before
   anything is fetched into them. */
static gboolean
request_make_directories (ResolveRequest *request)
{
  g_autoptr (GError) error = NULL;

  if (!aml_resolve_ensure_directory (request->libraries_dir, &error) ||
      !aml_resolve_ensure_directory (request->assets_dir, &error) ||
      !aml_resolve_ensure_directory (request->versions_dir, &error))
    {
      request_fail (request, g_steal_pointer (&error));
      return FALSE;
    }

  return TRUE;
}

AmlTaskScope *
aml_resolve_version_async (AmlVersionManifest *manifest,
                           const char         *version_id,
                           const char         *data_dir,
                           const char         *natives_dir,
                           const char         *asset_base_url,
                           AmlResolveProgressFunc progress,
                           gpointer              progress_data,
                           GAsyncReadyCallback   callback,
                           gpointer              user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  ResolveRequest *request;

  g_return_val_if_fail (AML_IS_VERSION_MANIFEST (manifest), NULL);
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
    AmlTaskScope *loading = aml_version_load_async (manifest, version_id,
                                                    version_loaded, request);

    aml_task_scope_follow_cancellable (loading, request->cancellable);
    aml_task_scope_unref_when_done (loading);
  }

  return g_steal_pointer (&scope);
}

AmlTaskScope *
aml_resolve_description_async (AmlVersionDescription *description,
                               const char             *version_id,
                               const char             *client_jar,
                               const char             *data_dir,
                               const char             *natives_dir,
                               const char             *asset_base_url,
                               AmlResolveProgressFunc   progress,
                               gpointer                progress_data,
                               GAsyncReadyCallback     callback,
                               gpointer                user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  ResolveRequest *request;

  g_return_val_if_fail (AML_IS_VERSION_DESCRIPTION (description), NULL);
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

AmlResolvedVersion *
aml_resolve_version_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
