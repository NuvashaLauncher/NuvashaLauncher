/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-loaders.h"
#include "nuvasha-loaders-private.h"

#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-minecraft-resolve.h"
#include "nuvasha-task-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * The document of one loader version
 * ---------------------------------------------------------- */

/* The libraries of a loader profile publish a maven address instead of a
 * download: a name and the repository the jar lives in. Turning that into
 * the download the address means is what lets the one resolve module
 * fetch everything from the library list of the merged document.
 *
 * @root is the parsed profile and is modified in place. */
gboolean
give_libraries_their_downloads (JsonNode *root, GError **error)
{
  JsonObject *object = json_node_get_object (root);
  JsonNode *libraries = json_object_get_member (object, "libraries");

  if (libraries == NULL || !JSON_NODE_HOLDS_ARRAY (libraries))
    return TRUE;

  {
    JsonArray *array = json_node_get_array (libraries);

    for (guint i = 0; i < json_array_get_length (array); i++)
      {
        JsonNode *entry = json_array_get_element (array, i);
        JsonObject *library = NULL;
        JsonNode *name = NULL;
        JsonNode *address = NULL;
        JsonObject *artifact = NULL;
        g_autofree char *path = NULL;
        g_autofree char *url = NULL;
        const char *coordinate = NULL;
        const char *repository = NULL;

        if (!JSON_NODE_HOLDS_OBJECT (entry))
          continue;

        library = json_node_get_object (entry);

        /* A library that already says where its jar comes from is left
           exactly as it is. */
        if (json_object_has_member (library, "downloads"))
          continue;

        name = json_object_get_member (library, "name");
        address = json_object_get_member (library, "url");

        if (name == NULL || !JSON_NODE_HOLDS_VALUE (name) ||
            json_node_get_value_type (name) != G_TYPE_STRING ||
            address == NULL || !JSON_NODE_HOLDS_VALUE (address) ||
            json_node_get_value_type (address) != G_TYPE_STRING)
          {
            g_set_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_MAVEN,
                         "a library of the loader profile names no jar to "
                         "download");
            return FALSE;
          }

        coordinate = json_node_get_string (name);
        repository = json_node_get_string (address);

        if (!g_str_has_prefix (repository, "https://"))
          {
            g_set_error (error, nuvasha_LOADERS_ERROR,
                         nuvasha_LOADERS_ERROR_NO_MAVEN,
                         "the loader repository is not an HTTPS address");
            return FALSE;
          }

        path = nuvasha_resolve_maven_path (coordinate);

        /* The path is written into the library directory later on, so it
           has to be a name and nothing else. */
        if (path == NULL || !nuvasha_resolve_entry_name_is_safe (path))
          {
            g_set_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_MAVEN,
                         "the coordinates \"%s\" are no library path", path);
            return FALSE;
          }

        url = g_strconcat (repository, path, NULL);

        artifact = json_object_new ();
        json_object_set_string_member (artifact, "path", path);
        json_object_set_string_member (artifact, "url", url);

        /* What the profile published about the bytes, when it published
           anything: sha1 is what the resolve module verifies. */
        {
          JsonNode *sha1 = json_object_get_member (library, "sha1");

          if (sha1 != NULL && JSON_NODE_HOLDS_VALUE (sha1) &&
              json_node_get_value_type (sha1) == G_TYPE_STRING)
            json_object_set_string_member (artifact, "sha1",
                                           json_node_get_string (sha1));
        }

        {
          JsonObject *downloads = json_object_new ();

          json_object_set_object_member (downloads, "artifact", artifact);
          json_object_set_object_member (library, "downloads", downloads);
        }
      }
  }

  return TRUE;
}

typedef struct {
  GTask *task;
  NuvashaTaskScope *scope;
} ProfileRequest;

static void
profile_request_free (gpointer data)
{
  ProfileRequest *request = data;

  g_clear_object (&request->task);
  g_free (request);
}

/* The document a profile is, with the downloads its libraries mean, in
   the shape nuvasha resolves. */
static NuvashaVersionDescription *
profile_from_root (JsonNode *root, GError **error)
{
  g_autoptr (JsonGenerator) generator = NULL;
  g_autofree char *data = NULL;
  gsize length = 0;

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_INSTALLER,
                   "the loader did not answer with a version document");
      return NULL;
    }

  if (!give_libraries_their_downloads (root, error))
    return NULL;

  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  data = json_generator_to_data (generator, &length);

  return nuvasha_version_description_new_from_json (data, length, error);
}

NuvashaVersionDescription *
nuvasha_loaders_profile_from_json (const char *json, gssize length,
                               GError **error)
{
  g_autoptr (JsonParser) parser = NULL;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, json, length, error))
    return NULL;

  return profile_from_root (json_parser_get_root (parser), error);
}

static void
profile_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  ProfileRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  NuvashaVersionDescription *description = NULL;

  parser = nuvasha_download_json_finish (result, &error);

  if (error == NULL)
    description = profile_from_root (json_parser_get_root (parser), &error);

  if (error != NULL)
    nuvasha_task_scope_return_error (request->scope, g_steal_pointer (&error));
  else
    nuvasha_task_scope_return_pointer (request->scope, description, g_object_unref);

  profile_request_free (request);
}

NuvashaTaskScope *
nuvasha_loaders_profile_async (NuvashaLoader           loader,
                           const char         *game_version,
                           const char         *loader_version,
                           GAsyncReadyCallback callback,
                           gpointer            user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  ProfileRequest *request = NULL;
  const char *base = NULL;
  g_autofree char *url = NULL;
  g_autofree char *quoted_game = NULL;
  g_autofree char *quoted_loader = NULL;

  g_return_val_if_fail (game_version != NULL, NULL);
  g_return_val_if_fail (loader_version != NULL, NULL);

  base = loader_meta (loader);
  quoted_game = g_uri_escape_string (game_version, NULL, FALSE);
  quoted_loader = g_uri_escape_string (loader_version, NULL, FALSE);

  request = g_new0 (ProfileRequest, 1);

  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->task = g_object_ref (nuvasha_task_scope_task (scope));
  request->scope = scope;

  nuvasha_task_scope_set_check_cancellable (request->scope, FALSE);

  if (base == NULL)
    {
      nuvasha_task_scope_return_error_deferred (
        request->scope,
        g_error_new (nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_UNSUPPORTED,
                     "%s publishes an installer instead of a version "
                     "document: nuvasha_loaders_install_async() is what "
                     "installs it", nuvasha_loader_display_name (loader)));
      profile_request_free (request);
      return g_steal_pointer (&scope);
    }

  /* The profile is a version document of its own that inherits from the
     Minecraft version: merging it over that version is what a loader
     instance runs. */
  url = g_strdup_printf ("%s/versions/loader/%s/%s/profile/json", base,
                         quoted_game, quoted_loader);

  nuvasha_task_scope_release_when_done (
    nuvasha_download_json_async (nuvasha_downloads_get_service (), url, profile_loaded, request),
    nuvasha_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

NuvashaVersionDescription *
nuvasha_loaders_profile_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
