/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-loaders.h"

#include "aml-download.h"
#include "aml-minecraft-resolve.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

GQuark
aml_loaders_error_quark (void)
{
  return g_quark_from_static_string ("aml-loaders-error");
}

/* ---------------------------------------------------------- *
 * Which loaders exist
 * ---------------------------------------------------------- */

/* A loader is in this list once it has been seen to start a game here,
   and the combo rows show nothing else. */
static const AmlLoader supported[] = {
  AML_LOADER_VANILLA,
  AML_LOADER_FABRIC,
  AML_LOADER_QUILT
};

#define SUPPORTED_COUNT (G_N_ELEMENTS (supported))

const AmlLoader *
aml_loaders_supported (void)
{
  return supported;
}

guint
aml_loaders_supported_count (void)
{
  return SUPPORTED_COUNT;
}

gboolean
aml_loader_is_supported (AmlLoader loader)
{
  for (guint i = 0; i < SUPPORTED_COUNT; i++)
    {
      if (supported[i] == loader)
        return TRUE;
    }

  return FALSE;
}

const char *
aml_loader_display_name (AmlLoader loader)
{
  switch (loader)
    {
    case AML_LOADER_VANILLA:
      return "Vanilla";

    case AML_LOADER_FABRIC:
      return "Fabric";

    case AML_LOADER_QUILT:
      return "Quilt";

    case AML_LOADER_FORGE:
      return "Forge";

    case AML_LOADER_NEOFORGE:
      return "NeoForge";
    }

  return "Vanilla";
}

void
aml_loader_version_free (AmlLoaderVersion *version)
{
  if (version == NULL)
    return;

  g_free (version->version);
  g_free (version);
}

/* ---------------------------------------------------------- *
 * Comparing versions
 * ---------------------------------------------------------- */

/* Compares two versions the way a version reads: every part that is a
 * number on both sides is compared as a number, everything else as text,
 * and a version with more parts is the newer one when the shorter is its
 * prefix. */
static int
compare_versions (const char *left, const char *right)
{
  g_auto (GStrv) left_parts = NULL;
  g_auto (GStrv) right_parts = NULL;
  guint i = 0;

  left_parts = g_strsplit (left, ".", -1);
  right_parts = g_strsplit (right, ".", -1);

  while (left_parts[i] != NULL && right_parts[i] != NULL)
    {
      char *left_end = NULL;
      char *right_end = NULL;
      glong left_number;
      glong right_number;
      int result;

      left_number = g_ascii_strtoll (left_parts[i], &left_end, 10);
      right_number = g_ascii_strtoll (right_parts[i], &right_end, 10);

      if (*left_end == '\0' && *right_end == '\0')
        result = (left_number > right_number) - (left_number < right_number);
      else
        result = g_strcmp0 (left_parts[i], right_parts[i]);

      if (result != 0)
        return result;

      i++;
    }

  if (left_parts[i] == NULL && right_parts[i] == NULL)
    return 0;

  return left_parts[i] == NULL ? -1 : 1;
}

/* TRUE when @version is a final release. A pre-release carries something
   after the numbers ("-beta.9", "-pre.3", "-rc.1"), and that is what a
   loader that publishes no flag of its own is read by. */
static gboolean
version_is_final (const char *version)
{
  const char *numbers = version;

  while (*numbers != '\0' && g_ascii_isdigit (*numbers))
    numbers++;

  for (const char *p = numbers; *p != '\0'; p++)
    {
      if (!g_ascii_isdigit (*p) && *p != '.')
        return FALSE;
    }

  return TRUE;
}

/* ---------------------------------------------------------- *
 * Where the loaders publish what
 * ---------------------------------------------------------- */

/* How @loader is reached. NULL for a loader this module does not know. */
static const char *
loader_base (AmlLoader loader)
{
  switch (loader)
    {
    case AML_LOADER_FABRIC:
      return AML_FABRIC_META_URL;

    case AML_LOADER_QUILT:
      return AML_QUILT_META_URL;

    default:
      return NULL;
    }
}

/* ---------------------------------------------------------- *
 * The versions a loader offers for a Minecraft version
 * ---------------------------------------------------------- */

typedef struct {
  GTask *task;
  GPtrArray *versions; /* AmlLoaderVersion*, element free aml_loader_version_free */
} VersionsRequest;

static void
versions_request_free (gpointer data)
{
  VersionsRequest *request = data;

  g_clear_object (&request->task);
  g_clear_pointer (&request->versions, g_ptr_array_unref);
  g_free (request);
}

/* What is unstable is what the loader says about it, and when it says
   nothing a pre-release version is what is not final. */
static gboolean
version_is_stable (JsonObject *loader, const char *version)
{
  JsonNode *stable;

  if (loader == NULL)
    return version_is_final (version);

  stable = json_object_get_member (loader, "stable");

  if (stable != NULL && JSON_NODE_HOLDS_VALUE (stable) &&
      json_node_get_value_type (stable) == G_TYPE_BOOLEAN)
    return json_node_get_boolean (stable);

  return version_is_final (version);
}

/* Reads one entry of a version list: {"loader": {"version": …}}. */
static AmlLoaderVersion *
version_from_entry (JsonNode *entry)
{
  JsonNode *loader = NULL;
  JsonObject *object = NULL;
  const char *version = NULL;
  AmlLoaderVersion *result = NULL;

  if (!JSON_NODE_HOLDS_OBJECT (entry))
    return NULL;

  object = json_node_get_object (entry);
  loader = json_object_get_member (object, "loader");

  if (loader == NULL || !JSON_NODE_HOLDS_OBJECT (loader))
    return NULL;

  {
    JsonNode *value = json_object_get_member (json_node_get_object (loader),
                                              "version");

    if (value == NULL || !JSON_NODE_HOLDS_VALUE (value) ||
        json_node_get_value_type (value) != G_TYPE_STRING)
      return NULL;

    version = json_node_get_string (value);
  }

  if (version == NULL || *version == '\0')
    return NULL;

  result = g_new0 (AmlLoaderVersion, 1);
  result->version = g_strdup (version);
  result->stable = version_is_stable (json_node_get_object (loader), version);

  return result;
}

/* Newest first: the loader answers in no particular order, and a list a
   user reads has to start with the version to install. */
static int
newest_first (gconstpointer left, gconstpointer right)
{
  const AmlLoaderVersion *one = *(AmlLoaderVersion *const *) left;
  const AmlLoaderVersion *other = *(AmlLoaderVersion *const *) right;

  return -compare_versions (one->version, other->version);
}

/* The versions out of what a loader answered. Nothing is filtered out
   here: what the user asked to see is decided where the list is shown. */
static GPtrArray *
versions_from_root (JsonNode *root, GError **error)
{
  g_autoptr (GPtrArray) versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_loader_version_free);

  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_VERSIONS,
                   "the loader did not answer with a list of versions");
      return NULL;
    }

  {
    JsonArray *array = json_node_get_array (root);

    for (guint i = 0; i < json_array_get_length (array); i++)
      {
        AmlLoaderVersion *version =
          version_from_entry (json_array_get_element (array, i));

        if (version != NULL)
          g_ptr_array_add (versions, version);
      }
  }

  if (versions->len == 0)
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_VERSIONS,
                   "this loader has no version for this Minecraft version");
      return NULL;
    }

  g_ptr_array_sort (versions, newest_first);

  return g_steal_pointer (&versions);
}

GPtrArray *
aml_loaders_versions_from_json (const char *json, gssize length,
                                GError **error)
{
  g_autoptr (JsonParser) parser = NULL;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, json, length, error))
    return NULL;

  return versions_from_root (json_parser_get_root (parser), error);
}

static void
versions_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  VersionsRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  GPtrArray *versions = NULL;

  parser = aml_download_json_finish (result, &error);

  if (error == NULL)
    versions = versions_from_root (json_parser_get_root (parser), &error);

  if (error != NULL)
    g_task_return_error (request->task, g_steal_pointer (&error));
  else
    g_task_return_pointer (request->task, versions,
                           (GDestroyNotify) g_ptr_array_unref);

  versions_request_free (request);
}

void
aml_loaders_versions_async (AmlLoader           loader,
                            const char         *game_version,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
  VersionsRequest *request = NULL;
  const char *base = NULL;
  g_autofree char *url = NULL;
  g_autofree char *quoted_game = NULL;

  g_return_if_fail (game_version != NULL);

  base = loader_base (loader);
  quoted_game = g_uri_escape_string (game_version, NULL, FALSE);

  request = g_new0 (VersionsRequest, 1);
  request->task = g_task_new (NULL, cancellable, callback, user_data);
  request->versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_loader_version_free);

  g_task_set_check_cancellable (request->task, FALSE);

  if (base == NULL)
    {
      g_task_return_new_error (request->task, AML_LOADERS_ERROR,
                               AML_LOADERS_ERROR_UNSUPPORTED,
                               "AML does not install %s",
                               aml_loader_display_name (loader));
      versions_request_free (request);
      return;
    }

  /* The loader answers with the versions that run this Minecraft
     version and nothing else, which is the compatibility filter. */
  url = g_strdup_printf ("%s/versions/loader/%s", base, quoted_game);

  aml_download_json_async (url, cancellable, versions_loaded, request);
}

GPtrArray *
aml_loaders_versions_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The document of one loader version
 * ---------------------------------------------------------- */

/* The libraries of a loader profile publish a maven address instead of a
 * download: a name and the repository the jar lives in. Turning that into
 * the download the address means is what lets the one resolve module
 * fetch everything from the library list of the merged document.
 *
 * @root is the parsed profile and is modified in place. */
static gboolean
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
            g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_MAVEN,
                         "a library of the loader profile names no jar to "
                         "download");
            return FALSE;
          }

        coordinate = json_node_get_string (name);
        repository = json_node_get_string (address);
        path = aml_resolve_maven_path (coordinate);

        /* The path is written into the library directory later on, so it
           has to be a name and nothing else. */
        if (path == NULL || !aml_resolve_entry_name_is_safe (path))
          {
            g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_MAVEN,
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
} ProfileRequest;

static void
profile_request_free (gpointer data)
{
  ProfileRequest *request = data;

  g_clear_object (&request->task);
  g_free (request);
}

/* The document a profile is, with the downloads its libraries mean, in
   the shape AML resolves. */
static AmlVersionDescription *
profile_from_root (JsonNode *root, GError **error)
{
  g_autoptr (JsonGenerator) generator = NULL;
  g_autofree char *data = NULL;
  gsize length = 0;

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the loader did not answer with a version document");
      return NULL;
    }

  if (!give_libraries_their_downloads (root, error))
    return NULL;

  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  data = json_generator_to_data (generator, &length);

  return aml_version_description_new_from_json (data, length, error);
}

AmlVersionDescription *
aml_loaders_profile_from_json (const char *json, gssize length,
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
  AmlVersionDescription *description = NULL;

  parser = aml_download_json_finish (result, &error);

  if (error == NULL)
    description = profile_from_root (json_parser_get_root (parser), &error);

  if (error != NULL)
    g_task_return_error (request->task, g_steal_pointer (&error));
  else
    g_task_return_pointer (request->task, description, g_object_unref);

  profile_request_free (request);
}

void
aml_loaders_profile_async (AmlLoader           loader,
                           const char         *game_version,
                           const char         *loader_version,
                           GCancellable       *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer            user_data)
{
  ProfileRequest *request = NULL;
  const char *base = NULL;
  g_autofree char *url = NULL;
  g_autofree char *quoted_game = NULL;
  g_autofree char *quoted_loader = NULL;

  g_return_if_fail (game_version != NULL);
  g_return_if_fail (loader_version != NULL);

  base = loader_base (loader);
  quoted_game = g_uri_escape_string (game_version, NULL, FALSE);
  quoted_loader = g_uri_escape_string (loader_version, NULL, FALSE);

  request = g_new0 (ProfileRequest, 1);
  request->task = g_task_new (NULL, cancellable, callback, user_data);

  g_task_set_check_cancellable (request->task, FALSE);

  if (base == NULL)
    {
      g_task_return_new_error (request->task, AML_LOADERS_ERROR,
                               AML_LOADERS_ERROR_UNSUPPORTED,
                               "AML does not install %s",
                               aml_loader_display_name (loader));
      profile_request_free (request);
      return;
    }

  /* The profile is a version document of its own that inherits from the
     Minecraft version: merging it over that version is what a loader
     instance runs. */
  url = g_strdup_printf ("%s/versions/loader/%s/%s/profile/json", base,
                         quoted_game, quoted_loader);

  aml_download_json_async (url, cancellable, profile_loaded, request);
}

AmlVersionDescription *
aml_loaders_profile_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}