/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-task-private.h"
#include "nuvashalauncher-loaders-private.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-i18n.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Which loaders exist
 * ---------------------------------------------------------- */

/* A loader is in this list once it has been seen to start a game here,
   and the combo rows show nothing else. */
static const nuvashalauncherLoader supported[] = {
  nuvashalauncher_LOADER_VANILLA,
  nuvashalauncher_LOADER_FABRIC,
  nuvashalauncher_LOADER_QUILT,
  nuvashalauncher_LOADER_NEOFORGE,
  nuvashalauncher_LOADER_FORGE
};

#define SUPPORTED_COUNT (G_N_ELEMENTS (supported))

const nuvashalauncherLoader *
nuvashalauncher_loaders_supported (void)
{
  return supported;
}

guint
nuvashalauncher_loaders_supported_count (void)
{
  return SUPPORTED_COUNT;
}

gboolean
nuvashalauncher_loader_is_supported (nuvashalauncherLoader loader)
{
  for (guint i = 0; i < SUPPORTED_COUNT; i++)
    {
      if (supported[i] == loader)
        return TRUE;
    }

  return FALSE;
}

const char *
nuvashalauncher_loader_display_name (nuvashalauncherLoader loader)
{
  switch (loader)
    {
    case nuvashalauncher_LOADER_VANILLA:
      return _("Vanilla");

    case nuvashalauncher_LOADER_FABRIC:
      return _("Fabric");

    case nuvashalauncher_LOADER_QUILT:
      return _("Quilt");

    case nuvashalauncher_LOADER_FORGE:
      return _("Forge");

    case nuvashalauncher_LOADER_NEOFORGE:
      return _("NeoForge");
    }

  return _("Vanilla");
}

void
nuvashalauncher_loader_version_free (nuvashalauncherLoaderVersion *version)
{
  if (version == NULL)
    return;

  g_free (version->version);
  g_free (version);
}

/* ---------------------------------------------------------- *
 * Comparing versions
 * ---------------------------------------------------------- */

/* Compares two versions the way a version reads: a run of digits is
 * compared as a number on both sides, so that "47.100" is newer than
 * "47.99", and everything else as text. */
static int
compare_versions (const char *left, const char *right)
{
  while (*left != '\0' && *right != '\0')
    {
      int result;

      if (g_ascii_isdigit (*left) && g_ascii_isdigit (*right))
        {
          const char *left_end = left;
          const char *right_end = right;
          const char *left_significant = left;
          const char *right_significant = right;
          gsize left_length;
          gsize right_length;

          while (g_ascii_isdigit (*left_end))
            left_end++;
          while (g_ascii_isdigit (*right_end))
            right_end++;

          while (left_significant < left_end && *left_significant == '0')
            left_significant++;
          while (right_significant < right_end && *right_significant == '0')
            right_significant++;

          left_length = left_end - left_significant;
          right_length = right_end - right_significant;

          if (left_length != right_length)
            result = left_length > right_length ? 1 : -1;
          else
            result = left_length == 0
                       ? 0
                       : memcmp (left_significant, right_significant,
                                 left_length);

          if (result != 0)
            return result;

          left = left_end;
          right = right_end;
          continue;
        }

      if (*left != *right)
        return (*left > *right) ? 1 : -1;

      left++;
      right++;
    }

  if (*left == *right)
    return 0;

  return (*left == '\0') ? -1 : 1;
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

/* How @loader is reached. NULL for a loader this module does not know,
 * and for the loaders that have no meta service: those are reached
 * through their own maven instead, see loader_maven(). */
const char *
loader_meta (nuvashalauncherLoader loader)
{
  switch (loader)
    {
    case nuvashalauncher_LOADER_FABRIC:
      return nuvashalauncher_FABRIC_META_URL;

    case nuvashalauncher_LOADER_QUILT:
      return nuvashalauncher_QUILT_META_URL;

    default:
      return NULL;
    }
}

/* The maven a loader publishes its installer and its artifacts in, and
 * where in it the loader itself lives. NULL for a loader that has no
 * maven. */
const char *
loader_maven (nuvashalauncherLoader loader, const char **coordinate)
{
  switch (loader)
    {
    case nuvashalauncher_LOADER_NEOFORGE:
      *coordinate = "net/neoforged/neoforge";
      return nuvashalauncher_NEOFORGE_MAVEN_URL;

    case nuvashalauncher_LOADER_FORGE:
      *coordinate = "net/minecraftforge/forge";
      return nuvashalauncher_FORGE_MAVEN_URL;

    default:
      *coordinate = NULL;
      return NULL;
    }
}

/* The host the version list API of a maven loader hangs off. It is not
 * always the base the artifacts live in: NeoForge keeps its repository
 * under /releases while its API is on the host itself, so asking
 * <repo>/api/... about it answers 404. NULL for a loader with no maven. */
static const char *
loader_maven_api (nuvashalauncherLoader loader)
{
  switch (loader)
    {
    case nuvashalauncher_LOADER_NEOFORGE:
      return nuvashalauncher_NEOFORGE_API_URL;

    case nuvashalauncher_LOADER_FORGE:
      return nuvashalauncher_FORGE_API_URL;

    default:
      return NULL;
    }
}

gboolean
nuvashalauncher_loader_publishes_an_installer (nuvashalauncherLoader loader)
{
  const char *coordinate = NULL;

  return loader_maven (loader, &coordinate) != NULL;
}

/* Separator between the Minecraft and loader versions. */
static char
loader_version_separator (nuvashalauncherLoader loader)
{
  return loader == nuvashalauncher_LOADER_FORGE ? '-' : '.';
}

/* The prefix NeoForge writes in front of its own numbers for a Minecraft
 * version. NeoForge drops the leading "1." of a modern Minecraft version
 * and writes a missing patch as 0: 1.21.1 is "21.1.", 1.21 is "21.0.".
 * A version that is not of the "1.x" shape, such as the snapshot "26.3",
 * is used as it is. */
static char *
neoforge_game_prefix (const char *game_version)
{
  const char *own = game_version;

  if (g_str_has_prefix (own, "1."))
    own += 2;

  if (strchr (own, '.') == NULL)
    return g_strdup_printf ("%s.0.", own);

  return g_strdup_printf ("%s.", own);
}

gboolean
nuvashalauncher_loaders_version_runs_game (nuvashalauncherLoader      loader,
                               const char    *loader_version,
                               const char    *game_version)
{
  g_autofree char *prefix = NULL;

  if (loader_version == NULL || game_version == NULL)
    return FALSE;

  /* A loader that has no maven names its versions after nothing, so the
     question is not asked of it. */
  if (!nuvashalauncher_loader_publishes_an_installer (loader))
    return FALSE;

  /* NeoForge names its versions after the Minecraft version in its own
     way, so the prefix is not just the version and a separator. */
  if (loader == nuvashalauncher_LOADER_NEOFORGE)
    prefix = neoforge_game_prefix (game_version);
  else
    prefix = g_strdup_printf ("%s%c", game_version,
                              loader_version_separator (loader));

  return g_str_has_prefix (loader_version, prefix);
}

/* Return the loader-owned portion of a Maven version string. */
static const char *
loader_version_own_part (nuvashalauncherLoader   loader,
                         const char *loader_version,
                         const char *game_version)
{
  gsize length;

  if (game_version == NULL)
    return loader_version;

  length = strlen (game_version);

  if (strncmp (loader_version, game_version, length) != 0)
    return loader_version;

  loader_version += length;

  if (*loader_version == loader_version_separator (loader))
    loader_version++;

  return loader_version;
}

/* ---------------------------------------------------------- *
 * The versions a loader offers for a Minecraft version
 * ---------------------------------------------------------- */

typedef struct {
  GTask *task;
  nuvashalauncherTaskScope *scope;
  nuvashalauncherLoader loader;
  char *game_version;
  GPtrArray *versions; /* nuvashalauncherLoaderVersion*, element free nuvashalauncher_loader_version_free */
} VersionsRequest;

static void
versions_request_free (gpointer data)
{
  VersionsRequest *request = data;

  g_clear_object (&request->task);
  g_clear_pointer (&request->versions, g_ptr_array_unref);
  g_free (request->game_version);
  g_free (request);
}

/* Reads one entry of a version list: {"loader": {"version": …}}. */
static nuvashalauncherLoaderVersion *
version_from_entry (JsonNode *entry)
{
  JsonNode *loader = NULL;
  JsonObject *object = NULL;
  const char *version = NULL;
  nuvashalauncherLoaderVersion *result = NULL;

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

  result = g_new0 (nuvashalauncherLoaderVersion, 1);
  result->version = g_strdup (version);
    result->stable = version_is_final (version);

  return result;
}

/* Newest first: the loader answers in no particular order, and a list a
   user reads has to start with the version to install. */
static int
newest_first (gconstpointer left, gconstpointer right)
{
  const nuvashalauncherLoaderVersion *one = *(nuvashalauncherLoaderVersion *const *) left;
  const nuvashalauncherLoaderVersion *other = *(nuvashalauncherLoaderVersion *const *) right;

  return -compare_versions (one->version, other->version);
}

/* The versions out of what a loader answered. Nothing is filtered out
   here: what the user asked to see is decided where the list is shown. */
static GPtrArray *
versions_from_root (JsonNode *root, GError **error)
{
  g_autoptr (GPtrArray) versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_loader_version_free);

  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS,
                   "the loader did not answer with a list of versions");
      return NULL;
    }

  {
    JsonArray *array = json_node_get_array (root);

    for (guint i = 0; i < json_array_get_length (array); i++)
      {
        nuvashalauncherLoaderVersion *version =
          version_from_entry (json_array_get_element (array, i));

        if (version != NULL)
          g_ptr_array_add (versions, version);
      }
  }

  if (versions->len == 0)
    {
      g_set_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS,
                   "this loader has no version for this Minecraft version");
      return NULL;
    }

  g_ptr_array_sort (versions, newest_first);

  return g_steal_pointer (&versions);
}

GPtrArray *
nuvashalauncher_loaders_versions_from_json (const char *json, gssize length,
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

GPtrArray *
nuvashalauncher_loaders_versions_for_display (GPtrArray  *published,
                                  gboolean    show_unstable,
                                  const char *wanted,
                                  guint      *selected)
{
  g_autoptr (GPtrArray) shown = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_loader_version_free);
  guint position = G_MAXUINT;

  g_return_val_if_fail (published != NULL, NULL);

  for (guint i = 0; i < published->len; i++)
    {
      const nuvashalauncherLoaderVersion *version = g_ptr_array_index (published, i);
      nuvashalauncherLoaderVersion *copy = NULL;

      /* The version an instance already runs stays in the list even
         when the user does not ask to see unstable versions. */
      if (wanted != NULL && g_str_equal (version->version, wanted))
        position = shown->len;
      else if (!version->stable && !show_unstable)
        continue;

      copy = g_new0 (nuvashalauncherLoaderVersion, 1);
      copy->version = g_strdup (version->version);
      copy->stable = version->stable;
      g_ptr_array_add (shown, copy);
    }

  /* Nothing was asked for, or what was asked for is not there: the
     newest stable version is what an install picks. The list is newest
     first, so the first stable one is the newest. A list that holds no
     stable version at all (a loader that has only beta releases for this
     Minecraft version, with unstable versions asked for) falls back to
     its newest, rather than leaving nothing picked. */
  if (position == G_MAXUINT)
    {
      for (guint i = 0; i < shown->len; i++)
        {
          const nuvashalauncherLoaderVersion *version = g_ptr_array_index (shown, i);

          if (version->stable)
            {
              position = i;
              break;
            }
        }

      if (position == G_MAXUINT && shown->len > 0)
        position = 0;
    }

  if (selected != NULL)
    *selected = position;

  return g_steal_pointer (&shown);
}

/* The versions a loader that has no meta service publishes: everything its
 * maven holds, as {"versions": ["..."]}, of which only the ones named
 * after the Minecraft version that was asked for are versions of it.
 *
 * What is not stable is what the loader published after its own numbers:
 * "26.3.0.51-beta" is a pre-release and "1.20.1-47.2.0" is a release, even
 * though both names carry a dash. */
static GPtrArray *
versions_from_maven (JsonNode     *root,
                     nuvashalauncherLoader     loader,
                     const char   *game_version,
                     GError      **error)
{
  g_autoptr (GPtrArray) versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_loader_version_free);
  JsonNode *node = NULL;
  JsonArray *array = NULL;

  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
    node = json_object_get_member (json_node_get_object (root), "versions");

  if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS,
                   "the loader did not answer with a list of versions");
      return NULL;
    }

  array = json_node_get_array (node);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *element = json_array_get_element (array, i);
      nuvashalauncherLoaderVersion *version = NULL;

      if (!JSON_NODE_HOLDS_VALUE (element) ||
          json_node_get_value_type (element) != G_TYPE_STRING)
        continue;

      if (!nuvashalauncher_loaders_version_runs_game (loader, json_node_get_string (element),
                                          game_version))
        continue;

      version = g_new0 (nuvashalauncherLoaderVersion, 1);
      version->version = g_strdup (json_node_get_string (element));
      version->stable = version_is_final (
        loader_version_own_part (loader, version->version, game_version));

      g_ptr_array_add (versions, version);
    }

  if (versions->len == 0)
    {
      g_set_error (error, nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_NO_VERSIONS,
                   "this loader has no version for this Minecraft version");
      return NULL;
    }

  g_ptr_array_sort (versions, newest_first);

  return g_steal_pointer (&versions);
}

GPtrArray *
nuvashalauncher_loaders_maven_versions_from_json (nuvashalauncherLoader      loader,
                                      const char    *game_version,
                                      const char    *json,
                                      gssize         length,
                                      GError       **error)
{
  g_autoptr (JsonParser) parser = NULL;

  g_return_val_if_fail (json != NULL, NULL);
  g_return_val_if_fail (game_version != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, json, length, error))
    return NULL;

  return versions_from_maven (json_parser_get_root (parser), loader,
                              game_version, error);
}

static void
versions_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  VersionsRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  JsonNode *root = NULL;
  GPtrArray *versions = NULL;

  parser = nuvashalauncher_download_json_finish (result, &error);

  if (error == NULL)
    {
      root = json_parser_get_root (parser);

      if (nuvashalauncher_loader_publishes_an_installer (request->loader))
        versions = versions_from_maven (root, request->loader,
                                        request->game_version, &error);
      else
        versions = versions_from_root (root, &error);
    }

  if (error != NULL)
    nuvashalauncher_task_scope_return_error (request->scope, g_steal_pointer (&error));
  else
    nuvashalauncher_task_scope_return_pointer (request->scope, versions,
                                   (GDestroyNotify) g_ptr_array_unref);

  versions_request_free (request);
}

nuvashalauncherTaskScope *
nuvashalauncher_loaders_versions_async (nuvashalauncherLoader           loader,
                            const char         *game_version,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  VersionsRequest *request = NULL;
  const char *meta = NULL;
  const char *api = NULL;
  const char *coordinate = NULL;
  g_autofree char *url = NULL;
  g_autofree char *quoted_game = NULL;

  g_return_val_if_fail (game_version != NULL, NULL);

  meta = loader_meta (loader);
  api = loader_maven_api (loader);
  quoted_game = g_uri_escape_string (game_version, NULL, FALSE);

  request = g_new0 (VersionsRequest, 1);

  /* Borrowed: the caller's handle keeps the operation alive, and the
     request is answered before it goes away. */
  request->task = g_object_ref (nuvashalauncher_task_scope_task (scope));
  request->scope = scope;
  request->loader = loader;
  request->game_version = g_strdup (game_version);
  request->versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_loader_version_free);

  nuvashalauncher_task_scope_set_check_cancellable (request->scope, FALSE);

  if (api != NULL)
    {
      /* The maven of the loader holds every version it ever published,
         for every Minecraft version; which of them runs the one that was
         asked for is said by their names, and that is the compatibility
         filter. The API hangs off the host, not off the repository path
         the artifacts live under. */
      loader_maven (loader, &coordinate);
      url = g_strdup_printf ("%s/api/maven/versions/releases/%s", api,
                             coordinate);
    }
  else if (meta != NULL)
    {
      url = g_strdup_printf ("%s/versions/loader/%s", meta, quoted_game);
    }
  else
    {
      nuvashalauncher_task_scope_return_error_deferred (
        request->scope,
        g_error_new (nuvashalauncher_LOADERS_ERROR, nuvashalauncher_LOADERS_ERROR_UNSUPPORTED,
                     "nuvashalauncher does not install %s",
                     nuvashalauncher_loader_display_name (loader)));
      versions_request_free (request);
      return g_steal_pointer (&scope);
    }

  nuvashalauncher_task_scope_release_when_done (
    nuvashalauncher_download_json_async (nuvashalauncher_downloads_get_service (), url, versions_loaded, request),
    nuvashalauncher_task_scope_get_cancellable (scope));

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvashalauncher_loaders_versions_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
