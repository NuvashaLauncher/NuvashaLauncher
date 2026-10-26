/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-minecraft-resolve-private.h"
#include <string.h>

#define NO_MAIN_CLASS_MESSAGE "this version document names no main class"

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
              /* Extract the placeholder name between the braces. */
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

/* Build the resolved version from the files on disk. */
nuvashalauncherResolvedVersion *
build_result (ResolveRequest *request, GError **error)
{
  nuvashalauncherResolvedVersion *resolved = g_new0 (nuvashalauncherResolvedVersion, 1);
  g_autoptr (GPtrArray) jvm = NULL;
  g_autoptr (GPtrArray) game = NULL;
  g_autoptr (GHashTable) known = g_hash_table_new (g_str_hash, g_str_equal);

  /* The request has a version description at this point. */
  if (request->description->main_class == NULL)
    {
      g_set_error (error, nuvashalauncher_RESOLVE_ERROR, nuvashalauncher_RESOLVE_ERROR_INVALID,
                   "%s", NO_MAIN_CLASS_MESSAGE);
      g_free (resolved);
      return NULL;
    }

  /* A loader that patches the client jar produced one already: it is the
     jar the game runs from and the final entry in the class path,
     which is where the class loader finds the classes of the game. */
  {
    const char *client_jar = request->client_jar_override != NULL
                               ? request->client_jar_override
                               : request->client_jar;

    resolved->client_jar = g_strdup (client_jar);
    resolved->classpath =
      nuvashalauncher_resolve_classpath (request->description, request->context,
                             request->libraries_dir, client_jar);
  }

  resolved->version_id = g_strdup (request->description->id);
  resolved->version_type = g_strdup (
    nuvashalauncher_mc_version_type_to_string (request->description->type));
  resolved->natives_directory = g_strdup (request->natives_dir);
  resolved->library_directory = g_strdup (request->libraries_dir);
  resolved->main_class = g_strdup (request->description->main_class);

  if (resolved->classpath == NULL)
    {
      nuvashalauncher_resolved_version_free (resolved);
      return NULL;
    }

  if (request->description->java_version != NULL)
    resolved->java_major_version =
      request->description->java_version->major_version;

  /* A virtual index is a directory the game reads its assets from
     directly, so that directory is used as its assets root
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

  jvm = nuvashalauncher_resolve_jvm_arguments (request->description, request->context);
  game = nuvashalauncher_resolve_game_arguments (request->description, request->context);

  resolved->jvm_arguments = expand_arguments (jvm, known);
  resolved->game_arguments = expand_arguments (game, known);

  return resolved;
}

