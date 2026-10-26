#define _GNU_SOURCE

/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-loaders-private.h"
#include "nuvashalauncher-task-private.h"
#include "nuvashalauncher-fs.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-minecraft-resolve.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

GQuark
nuvashalauncher_loaders_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-loaders-error");
}

static void
install_file_free (gpointer data)
{
  InstallFile *file = data;

  g_free (file->url);
  g_free (file->entry);
  g_free (file->path);
  g_free (file->sha1);
  g_free (file);
}


static void
install_request_free (gpointer data)
{
  InstallRequest *request = data;

  g_clear_object (&request->task);
  g_clear_object (&request->profile);
  g_clear_object (&request->description);
  g_clear_pointer (&request->processors, g_ptr_array_unref);
  g_clear_pointer (&request->pending, g_ptr_array_unref);
  g_clear_pointer (&request->variables, g_hash_table_unref);
  g_free (request->data_dir);
  g_free (request->game_version);
  g_free (request->loader_version);
  g_free (request->java);
  g_free (request->root);
  g_free (request->libraries_dir);
  g_free (request->installer);
  g_free (request->client_jar);
  g_free (request->patched_jar);
  g_free (request->main_class);
  g_free (request);
}

/* Ends the install. Consumes @error, which may be NULL.
 *
 * The answer is deferred: the constructor itself can find out that a
 * loader publishes no installer, and the callback must not run before the
 * caller has been handed the handle it is meant to release. */
static void
install_finished (InstallRequest *request, GError *error)
{
  if (error != NULL)
    {
      nuvashalauncher_task_scope_return_error_deferred (request->scope, error);
    }
  else
    {
      nuvashalauncherLoaderInstall *install = g_new0 (nuvashalauncherLoaderInstall, 1);

      install->description = g_object_ref (request->description);
      install->client_jar = g_strdup (request->patched_jar);

      nuvashalauncher_task_scope_return_pointer_deferred (
        request->scope, install, (GDestroyNotify) nuvashalauncher_loader_install_free);
    }

  install_request_free (request);
}

void
report_progress (InstallRequest *request)
{
  if (request->progress == NULL)
    return;

  request->progress ("installing the loader", request->done,
                     request->done + request->pending->len,
                     request->progress_data);
}

void
install_failed (InstallRequest *request, GError *error)
{
  install_finished (request, error);
}

/* ---------------------------------------------------------- *
 * The files an install needs
 * ---------------------------------------------------------- */

static void
queue_file (InstallRequest *request, InstallFileKind kind, const char *url,
            const char *entry, const char *path, const char *sha1,
            gboolean optional)
{
  InstallFile *file = g_new0 (InstallFile, 1);

  file->kind = kind;
  file->url = g_strdup (url);
  file->entry = g_strdup (entry);
  file->path = g_strdup (path);
  file->sha1 = g_strdup (sha1);
  file->optional = optional;

  g_ptr_array_add (request->pending, file);
}

/* Where the artifact @coordinate of the maven of the loader belongs. NULL
 * when the coordinates make no path, which the caller refuses. */
char *
artifact_path (InstallRequest *request, const char *coordinate)
{
  g_autofree char *relative = NULL;

  relative = nuvashalauncher_resolve_maven_path (coordinate);

  if (relative == NULL || !nuvashalauncher_resolve_entry_name_is_safe (relative))
    return NULL;

  return g_build_filename (request->libraries_dir, relative, NULL);
}

/* Queue the artifact named by @coordinate from the loader's Maven
 * repository. Existing files are reused; the repository is the source of
 * truth because loader metadata does not provide hashes.
 *
 * @optional marks processor outputs that may not exist in Maven. */
void
queue_artifact (InstallRequest *request, const char *coordinate,
                gboolean optional)
{
  const char *maven = NULL;
  const char *name = NULL;
  g_autofree char *relative = NULL;
  g_autofree char *path = NULL;
  g_autofree char *url = NULL;

  maven = loader_maven (request->loader, &name);

  if (maven == NULL)
    return;

  relative = nuvashalauncher_resolve_maven_path (coordinate);

  if (relative == NULL || !nuvashalauncher_resolve_entry_name_is_safe (relative))
    return;

  path = g_build_filename (request->libraries_dir, relative, NULL);

  {
    GStatBuf info;

    /* This pathname becomes a Java classpath entry. Do not treat a planted
     * symlink as an already fetched artifact. */
    if (g_lstat (path, &info) == 0 && S_ISREG (info.st_mode))
      {
        int fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0)
          {
            close (fd);
            return;
          }
      }
  }

  url = g_strconcat (maven, "/", relative, NULL);
  queue_file (request, FILE_FETCH, url, NULL, path, NULL, optional);
}

/* The file @entry of the installer jar, such as the patch the processors
 * apply, written out where the processors can find it. */
void
queue_unpacked (InstallRequest *request, const char *entry)
{
  g_autofree char *path = NULL;

  if (entry == NULL || *entry == '\0')
    return;

  /* The profile writes the paths inside its own jar with a leading
     slash, and this is what an entry has to look like to be one. */
  while (*entry == '/')
    entry++;

  if (*entry == '\0' || strstr (entry, "..") != NULL ||
      !nuvashalauncher_resolve_entry_name_is_safe (entry))
    return;

  path = g_build_filename (request->root, entry, NULL);

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return;

  queue_file (request, FILE_UNPACK, NULL, entry, path, NULL, FALSE);
}


static void
file_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  InstallRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  InstallFile *file = g_ptr_array_index (request->pending, 0);

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      /* A maven that does not hold an artifact a name of the profile says
         nothing about: only the processors know, and they are run next. */
      if (file->optional && g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                                             nuvashalauncher_DOWNLOAD_ERROR_HTTP))
        g_clear_error (&error);
      else
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }
    }

  g_ptr_array_remove_index (request->pending, 0);
  request->done++;

  report_progress (request);
  drain_pending (request);
}

void
drain_pending (InstallRequest *request)
{
  InstallFile *file = NULL;
  g_autofree char *directory = NULL;
  g_autoptr (GError) error = NULL;

  if (request->pending->len == 0)
    {
      install_step_done (request);
      return;
    }

  file = g_ptr_array_index (request->pending, 0);
  directory = g_path_get_dirname (file->path);

  if (!nuvashalauncher_loader_ensure_directory (directory, &error))
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  if (file->kind == FILE_UNPACK)
    {
      g_autoptr (GBytes) bytes =
        nuvashalauncher_loader_read_zip_entry (request->installer, file->entry, &error);
      const char *data = NULL;
      gsize size = 0;

      if (bytes == NULL)
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }

      data = g_bytes_get_data (bytes, &size);

      if (!nuvashalauncher_loader_write_install_file (file->path, data, size, &error))
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }

      g_ptr_array_remove_index (request->pending, 0);
      request->done++;

      report_progress (request);
      drain_pending (request);
      return;
    }

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), file->url, file->path, file->sha1,
                             file->sha1 != NULL ? G_CHECKSUM_SHA1
                                                : G_CHECKSUM_MD5,
                             NULL, file_ready, request),
    g_task_get_cancellable (request->task));
}




/* ---------------------------------------------------------- *
 * The steps of an install
 * ---------------------------------------------------------- */

static void
stage_installer (InstallRequest *request)
{
  const char *maven = NULL;
  const char *coordinate = NULL;
  g_autofree char *url = NULL;
  g_autofree char *file = NULL;

  request->stage = STAGE_INSTALLER;

  maven = loader_maven (request->loader, &coordinate);

  if (maven == NULL || coordinate == NULL)
    {
      install_failed (request,
                      g_error_new (nuvashalauncher_LOADERS_ERROR,
                                   nuvashalauncher_LOADERS_ERROR_UNSUPPORTED,
                                   "nuvashalauncher does not install %s",
                                   nuvashalauncher_loader_display_name (request->loader)));
      return;
    }

  /* Every loader publishes its installer under the name of its own
     artifact: neoforge-<version>-installer.jar, forge-… . */
  {
    g_autofree char *artifact_base = g_path_get_basename (coordinate);

    file = g_strdup_printf ("%s-%s-installer.jar", artifact_base,
                            request->loader_version);
  }
  url = g_strdup_printf ("%s/%s/%s/%s", maven, coordinate,
                         request->loader_version, file);

  queue_file (request, FILE_FETCH, url, NULL, request->installer, NULL, FALSE);
  drain_pending (request);
}

/* TRUE when the coordinate @name is a tool one of the processors runs
 * with. Those are what installs the loader, not what runs the game with
 * it, so they are kept off the class path of the game. */
static gboolean
is_a_processor_tool (InstallRequest *request, const char *name)
{
  for (guint i = 0; i < request->processors->len; i++)
    {
      JsonObject *processor =
        json_node_get_object (g_ptr_array_index (request->processors, i));
      JsonNode *node = json_object_get_member (processor, "jar");

      if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
          json_node_get_value_type (node) == G_TYPE_STRING &&
          g_str_equal (json_node_get_string (node), name))
        return TRUE;

      node = json_object_get_member (processor, "classpath");

      if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
        continue;

      {
        JsonArray *array = json_node_get_array (node);

        for (guint j = 0; j < json_array_get_length (array); j++)
          {
            JsonNode *entry = json_array_get_element (array, j);

            if (JSON_NODE_HOLDS_VALUE (entry) &&
                json_node_get_value_type (entry) == G_TYPE_STRING &&
                g_str_equal (json_node_get_string (entry), name))
              return TRUE;
          }
      }
    }

  return FALSE;
}

/* The document of the loader, with the libraries of its own install
 * profile added to it.
 *
 * Those are the jars the loader runs the game with and the document does
 * not name: the loader itself is one of them, and it is the jar FML looks
 * for when it checks that a NeoForge installation is whole. The ones the
 * document already names are left as they are, and the tools of the
 * processors are left out, because they install the loader rather than
 * run the game with it. */
static nuvashalauncherVersionDescription *
document_with_install_libraries (InstallRequest *request, const char *json,
                                 gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (JsonGenerator) generator = NULL;
  g_autoptr (nuvashalauncherVersionDescription) document = NULL;
  g_autofree char *data = NULL;
  gsize written = 0;
  JsonNode *root = NULL;
  JsonObject *object = NULL;
  JsonNode *libraries = NULL;
  JsonArray *array = NULL;
  JsonNode *installed = NULL;

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, json, length, error))
    return NULL;

  root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_INSTALLER,
                   "the version document of %s %s is not a document nuvashalauncher can "
                   "read", nuvashalauncher_loader_display_name (request->loader),
                   request->loader_version);
      return NULL;
    }

  object = json_node_get_object (root);
  libraries = json_object_get_member (object, "libraries");

  /* A document with no libraries at all still gets a list to add them
     to. */
  if (libraries == NULL || !JSON_NODE_HOLDS_ARRAY (libraries))
    {
      JsonNode *empty = json_node_new (JSON_NODE_ARRAY);

      json_node_take_array (empty, json_array_new ());
      json_object_set_member (object, "libraries", empty);
      libraries = empty;
    }

  array = json_node_get_array (libraries);
  installed = json_object_get_member (install_profile (request), "libraries");

  if (installed != NULL && JSON_NODE_HOLDS_ARRAY (installed))
    {
      JsonArray *installed_array = json_node_get_array (installed);

      for (guint i = 0; i < json_array_get_length (installed_array); i++)
        {
          JsonNode *entry = json_array_get_element (installed_array, i);
          JsonNode *name = NULL;
          gboolean known = FALSE;

          if (!JSON_NODE_HOLDS_OBJECT (entry))
            continue;

          name = json_object_get_member (json_node_get_object (entry), "name");

          if (name == NULL || !JSON_NODE_HOLDS_VALUE (name) ||
              json_node_get_value_type (name) != G_TYPE_STRING)
            continue;

          for (guint j = 0; j < json_array_get_length (array); j++)
            {
              JsonNode *known_entry = json_array_get_element (array, j);
              JsonNode *known_name = NULL;

              if (!JSON_NODE_HOLDS_OBJECT (known_entry))
                continue;

              known_name = json_object_get_member (
                json_node_get_object (known_entry), "name");

              if (known_name != NULL && JSON_NODE_HOLDS_VALUE (known_name) &&
                  json_node_get_value_type (known_name) == G_TYPE_STRING &&
                  g_str_equal (json_node_get_string (known_name),
                               json_node_get_string (name)))
                known = TRUE;
            }

          if (known || is_a_processor_tool (request, json_node_get_string (name)))
            continue;

          json_array_add_element (array, json_node_copy (entry));
        }
    }

  /* What the added libraries publish as a maven address rather than as a
     download becomes the download it means, which is the same work the
     profiles of the other loaders need. */
  if (!give_libraries_their_downloads (root, error))
    return NULL;

  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  data = json_generator_to_data (generator, &written);

  document = nuvashalauncher_version_description_new_from_json (data, written, error);

  return g_steal_pointer (&document);
}

/* The install profile out of the installer jar, and the document the
 * loader runs the game with, which is in the same jar. */
static void
stage_profile (InstallRequest *request)
{
  g_autoptr (GBytes) profile_bytes = NULL;
  g_autoptr (GBytes) document_bytes = NULL;
  g_autoptr (nuvashalauncherVersionDescription) description = NULL;
  g_autoptr (GError) error = NULL;
  JsonParser *parser = NULL;
  const char *json_entry = "version.json";
  JsonNode *node = NULL;

  request->stage = STAGE_PROFILE;

  profile_bytes =
    nuvashalauncher_loader_read_zip_entry (request->installer, "install_profile.json", &error);

  if (profile_bytes == NULL)
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (profile_bytes, NULL),
                                   (gssize) g_bytes_get_size (profile_bytes),
                                   &error))
    {
      g_object_unref (parser);
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  node = json_parser_get_root (parser);

  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    {
      g_object_unref (parser);
      install_failed (request,
                      g_error_new (nuvashalauncher_LOADERS_ERROR,
                                   nuvashalauncher_LOADERS_ERROR_INSTALLER,
                                   "the install profile of %s %s is not a "
                                   "document nuvashalauncher can read",
                                   nuvashalauncher_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  /* Keep the parser alive because processor nodes belong to its profile. */
  request->profile = parser;

  /* The processors of the client side: the ones that name no side at all,
     and the ones that name this one. They are parts of the profile, which
     the parser keeps, and the document below is read with them known. */
  request->processors = g_ptr_array_new ();

  node = json_object_get_member (install_profile (request), "processors");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          JsonNode *sides = NULL;
          gboolean wanted = TRUE;

          if (!JSON_NODE_HOLDS_OBJECT (element))
            continue;

          sides = json_object_get_member (json_node_get_object (element),
                                          "sides");

          if (sides != NULL && JSON_NODE_HOLDS_ARRAY (sides))
            {
              JsonArray *side_array = json_node_get_array (sides);

              wanted = FALSE;

              for (guint j = 0; j < json_array_get_length (side_array); j++)
                {
                  JsonNode *side = json_array_get_element (side_array, j);

                  if (JSON_NODE_HOLDS_VALUE (side) &&
                      json_node_get_value_type (side) == G_TYPE_STRING &&
                      g_str_equal (json_node_get_string (side), "client"))
                    wanted = TRUE;
                }
            }

          if (wanted)
            g_ptr_array_add (request->processors, element);
        }
    }

  if (request->processors->len == 0)
    {
      install_failed (request,
                      g_error_new (nuvashalauncher_LOADERS_ERROR,
                                   nuvashalauncher_LOADERS_ERROR_GENERATION,
                                   "the install profile of %s %s runs no "
                                   "processor for the client, which is not a "
                                   "shape nuvashalauncher knows how to install",
                                   nuvashalauncher_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  /* The document of the loader is in the jar as well, and the profile says
     under what name. */
  node = json_object_get_member (install_profile (request), "json");

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    json_entry = json_node_get_string (node);

  while (*json_entry == '/')
    json_entry++;

  document_bytes = nuvashalauncher_loader_read_zip_entry (request->installer, json_entry, &error);

  if (document_bytes == NULL)
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  description = document_with_install_libraries (
    request, g_bytes_get_data (document_bytes, NULL),
    (gssize) g_bytes_get_size (document_bytes), &error);

  if (description == NULL)
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  request->description = g_steal_pointer (&description);

  install_step_done (request);
}


/* The patched client jar is what the game starts from, and the loader of
 * the game reads one thing out of it before it loads anything: which
 * distribution it is. What the processors of this generation of the
 * loader leave out is that one line of manifest, and without it the game
 * answers that the installation is corrupted and stops there. */
static void
install_done (InstallRequest *request)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *dists = NULL;

  if (request->patched_jar == NULL ||
      !g_file_test (request->patched_jar, G_FILE_TEST_IS_REGULAR))
    {
      install_failed (request,
                      g_error_new (nuvashalauncher_LOADERS_ERROR,
                                   nuvashalauncher_LOADERS_ERROR_GENERATION,
                                   "the processors of %s %s did not leave a "
                                   "patched client jar behind, which is not a "
                                   "shape nuvashalauncher knows how to install",
                                   nuvashalauncher_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  dists = nuvashalauncher_loader_jar_manifest_attribute (request->patched_jar, "Minecraft-Dists");

  if (dists == NULL)
    dists = nuvashalauncher_loader_jar_manifest_attribute (request->patched_jar, "Minecraft-Dist");

  if (dists == NULL)
    {
      if (!nuvashalauncher_loader_write_jar_manifest_attribute (request->patched_jar,
                                         "Minecraft-Dists", "client",
                                         &error))
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }
    }

  install_finished (request, NULL);
}

void
install_step_done (InstallRequest *request)
{
  switch (request->stage)
    {
    case STAGE_CLIENT_JAR:
      stage_installer (request);
      break;

    case STAGE_INSTALLER:
      stage_profile (request);
      break;

    case STAGE_PROFILE:
      request->index = 0;
      stage_processor (request);
      break;

    case STAGE_VARIABLES:
      stage_run (request);
      break;

    case STAGE_RUN:
      /* One processor has run: the next one, or the document. */
      request->index++;

      if (request->index < request->processors->len)
        stage_processor (request);
      else
        install_done (request);
      break;

    case STAGE_DONE:
      break;
    }
}

nuvashalauncherTaskScope *
nuvashalauncher_loaders_install_async (nuvashalauncherLoader              loader,
                           const char            *game_version,
                           const char            *loader_version,
                           nuvashalauncherVersionDescription *game_document,
                           const char            *java_executable,
                           const char            *data_dir,
                           nuvashalauncherLoadersProgressFunc  progress,
                           gpointer                progress_data,
                           GAsyncReadyCallback     callback,
                           gpointer                user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  InstallRequest *request = NULL;
  g_autofree char *versions_dir = NULL;
  g_autofree char *jar_name = NULL;

  g_return_val_if_fail (game_version != NULL, NULL);
  g_return_val_if_fail (loader_version != NULL, NULL);
  g_return_val_if_fail (nuvashalauncher_IS_VERSION_DESCRIPTION (game_document), NULL);
  g_return_val_if_fail (java_executable != NULL, NULL);

  request = g_new0 (InstallRequest, 1);

  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->task = g_object_ref (nuvashalauncher_task_scope_task (scope));
  request->scope = scope;
  request->loader = loader;
  request->game_version = g_strdup (game_version);
  request->loader_version = g_strdup (loader_version);
  request->java = g_strdup (java_executable);
  request->pending = g_ptr_array_new_with_free_func (install_file_free);
  request->progress = progress;
  request->progress_data = progress_data;

  g_task_set_check_cancellable (request->task, FALSE);

  if (!nuvashalauncher_loader_publishes_an_installer (loader))
    {
      install_failed (request,
                      g_error_new (nuvashalauncher_LOADERS_ERROR,
                                   nuvashalauncher_LOADERS_ERROR_UNSUPPORTED,
                                   "%s publishes no installer to run",
                                   nuvashalauncher_loader_display_name (loader)));
      return g_steal_pointer (&scope);
    }

  if (data_dir != NULL)
    request->data_dir = g_strdup (data_dir);
  else
    request->data_dir = g_build_filename (g_get_user_data_dir (), "nuvashalauncher", NULL);

  /* Every loader version is installed in a directory of its own, with the
     artifacts of its profile in a maven beside it, so that the games and
     the installers share nothing and an unused version can be thrown away
     whole. */
  request->root = g_build_filename (request->data_dir, nuvashalauncher_LOADERS_SUBDIR,
                                    nuvashalauncher_loader_to_string (loader),
                                    loader_version, NULL);

  /* The artifacts of a profile go into the library directory of the games,
     which is where the document of the loader names them: the patched
     client jar among them, by its own coordinates. */
  request->libraries_dir = g_build_filename (request->data_dir, "libraries",
                                             NULL);

  jar_name = g_strdup_printf ("%s-%s-installer.jar",
                              nuvashalauncher_loader_to_string (loader), loader_version);
  request->installer = g_build_filename (request->root, jar_name, NULL);

  /* The client jar of the game version is what the processors patch, and
     the install fetches that one file of the game itself: the resolve
     module is never asked for the game of a loader, only for the loader
     document over it. */
  if (game_document->client == NULL || game_document->client->url == NULL)
    {
      install_failed (request,
                      g_error_new (nuvashalauncher_LOADERS_ERROR,
                                   nuvashalauncher_LOADERS_ERROR_INSTALLER,
                                   "Minecraft %s has no client jar to patch",
                                   game_version));
      return g_steal_pointer (&scope);
    }

  versions_dir = g_build_filename (request->data_dir, "versions",
                                   game_version, NULL);
  jar_name = g_strdup_printf ("%s.jar", game_version);
  request->client_jar = g_build_filename (versions_dir, jar_name, NULL);

  request->stage = STAGE_CLIENT_JAR;

  if (g_file_test (request->client_jar, G_FILE_TEST_IS_REGULAR))
    {
      stage_installer (request);
      return g_steal_pointer (&scope);
    }

  queue_file (request, FILE_FETCH, game_document->client->url, NULL,
              request->client_jar, game_document->client->sha1, FALSE);
  drain_pending (request);

  return g_steal_pointer (&scope);
}

nuvashalauncherLoaderInstall *
nuvashalauncher_loaders_install_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
