/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Path planning, compatibility filtering and argument expansion for a
 * resolved Minecraft version. Network and archive work remain separate. */

#include "nuvasha-minecraft-resolve-private.h"

#include "nuvasha-fs.h"

#include <string.h>

#define CLASSPATH_SEPARATOR ":"

/* A version id or a file name from a document is only a file name after
   everything a file name cannot hold has been taken out of it. */
char *
nuvasha_resolve_safe_name (const char *id)
{
  char *name = g_strdup (id);

  for (char *p = name; *p != '\0'; p++)
    {
      if (!g_ascii_isalnum (*p) && *p != '.' && *p != '-' && *p != '_')
        *p = '_';
    }

  return name;
}

gboolean
nuvasha_resolve_ensure_directory (const char *path, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  if (!nuvasha_fs_make_directory (path, 0755, &local_error))
    {
      g_set_error (error, nuvasha_RESOLVE_ERROR, nuvasha_RESOLVE_ERROR_IO,
                   "cannot create \"%s\": %s", path,
                   local_error->message);
      return FALSE;
    }

  return TRUE;
}

/* Creates the directory @path lives in. */
gboolean
nuvasha_resolve_ensure_parent_directory (const char *path, GError **error)
{
  g_autofree char *parent = g_path_get_dirname (path);

  return nuvasha_resolve_ensure_directory (parent, error);
}

/* ---------------------------------------------------------- *
 * Maven paths
 * ---------------------------------------------------------- */

/* TRUE when @part can be one component of a coordinate: nothing that
   would step out of the libraries directory when it becomes a path. */
static gboolean
is_coordinate_part (const char *part)
{
  if (part == NULL || part[0] == '\0')
    return FALSE;

  if (g_str_equal (part, ".") || g_str_equal (part, ".."))
    return FALSE;

  return strchr (part, '/') == NULL && strchr (part, '\\') == NULL;
}

/* The maven path of @coordinate, with @extra_classifier used when the
   coordinate does not name one itself. */
static char *
maven_path (const char *coordinate, const char *extra_classifier)
{
  g_auto (GStrv) parts = NULL;
  g_autofree char *classifier = NULL;
  g_autofree char *extension = NULL;
  g_autofree char *group_path = NULL;
  g_autofree char *version = NULL;
  g_autofree char *file = NULL;
  const char *at = NULL;
  guint count;

  if (coordinate == NULL)
    return NULL;

  parts = g_strsplit (coordinate, ":", -1);
  count = g_strv_length (parts);

  /* group:artifact:version, with an optional classifier. */
  if (count < 3 || count > 4)
    return NULL;

  if (!is_coordinate_part (parts[0]) || !is_coordinate_part (parts[1]))
    return NULL;

  /* The extension is written after an "@" on the last part of the
     coordinate, which is the version when there is no classifier and
     the classifier when there is one. */
  version = g_strdup (parts[2]);

  if (count == 4)
    {
      if (!is_coordinate_part (parts[3]))
        return NULL;

      classifier = g_strdup (parts[3]);
    }
  else if (extra_classifier != NULL && extra_classifier[0] != '\0')
    {
      if (!is_coordinate_part (extra_classifier))
        return NULL;

      classifier = g_strdup (extra_classifier);
    }

  at = strchr (classifier != NULL ? classifier : version, '@');

  if (at != NULL)
    {
      *((char *) at) = '\0';
      extension = g_strdup (at + 1);

      if (!is_coordinate_part (extension))
        return NULL;
    }

  if (!is_coordinate_part (version) ||
      (classifier != NULL && !is_coordinate_part (classifier)))
    return NULL;

  group_path = g_strdup (parts[0]);

  for (char *p = group_path; *p != '\0'; p++)
    {
      if (*p == '.')
        *p = '/';
    }

  if (classifier != NULL)
    file = g_strdup_printf ("%s-%s-%s.%s", parts[1], version, classifier,
                            extension != NULL ? extension : "jar");
  else
    file = g_strdup_printf ("%s-%s.%s", parts[1], version,
                            extension != NULL ? extension : "jar");

  {
    g_autofree char *directory =
      g_build_filename (group_path, parts[1], version, NULL);

    return g_build_filename (directory, file, NULL);
  }
}

char *
nuvasha_resolve_maven_path (const char *coordinate)
{
  return maven_path (coordinate, NULL);
}

char *
nuvasha_resolve_maven_path_classified (const char *coordinate,
                                   const char *classifier)
{
  return maven_path (coordinate, classifier);
}

/* ---------------------------------------------------------- *
 * Choosing the natives of a library
 * ---------------------------------------------------------- */

static const char *const *
arch_spellings (const char *arch)
{
  static const char *const x86_64[] = { "x86_64", "x86-64", "amd64", NULL };
  static const char *const aarch64[] = { "arm64", "aarch64", NULL };
  static const char *const x86[] = { "x86", "i386", "i686", NULL };
  static const char *const none[] = { NULL };

  if (arch == NULL)
    return none;

  if (g_str_equal (arch, "x86_64") || g_str_equal (arch, "amd64"))
    return x86_64;

  if (g_str_equal (arch, "aarch64") || g_str_equal (arch, "arm64"))
    return aarch64;

  if (g_str_equal (arch, "x86") || g_str_equal (arch, "i386") ||
      g_str_equal (arch, "i686"))
    return x86;

  return none;
}

const char *
nuvasha_resolve_native_classifier (const NuvashaLibrary       *library,
                               const NuvashaRuleContext *context)
{
  const char *value = NULL;
  const char *const *spellings = NULL;
  GHashTableIter iter;
  gpointer key = NULL;

  g_return_val_if_fail (library != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);

  /* Without a jar published under a classifier there are no natives to
     pick, whatever the tables say. */
  if (library->classifiers == NULL)
    return NULL;

  if (library->natives != NULL)
    value = g_hash_table_lookup (library->natives, context->os_name);

  if (value != NULL && g_hash_table_contains (library->classifiers, value))
    return value;

  /* One jar per machine: the architecture is part of the name. The key
     of the table is returned, which belongs to the library. */
  spellings = arch_spellings (context->os_arch);

  if (spellings[0] == NULL)
    return NULL;

  g_hash_table_iter_init (&iter, library->classifiers);

  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      for (guint i = 0; spellings[i] != NULL; i++)
        {
          g_autofree char *name =
            g_strdup_printf ("natives-%s-%s", context->os_name, spellings[i]);

          if (g_strcmp0 (key, name) == 0)
            return key;
        }
    }

  return NULL;
}

gboolean
nuvasha_resolve_entry_name_is_safe (const char *entry_name)
{
  g_auto (GStrv) parts = NULL;

  if (entry_name == NULL || entry_name[0] == '\0')
    return FALSE;

  /* An absolute name would ignore the directory it is written into. */
  if (entry_name[0] == '/' || strchr (entry_name, '\\') != NULL)
    return FALSE;

  parts = g_strsplit (entry_name, "/", -1);

  for (guint i = 0; parts[i] != NULL; i++)
    {
      if (parts[i][0] == '\0' || g_str_equal (parts[i], ".") ||
          g_str_equal (parts[i], ".."))
        return FALSE;
    }

  return TRUE;
}

/* ---------------------------------------------------------- *
 * The files a version needs
 * ---------------------------------------------------------- */

void
nuvasha_resolve_file_free (NuvashaResolveFile *file)
{
  if (file == NULL)
    return;

  g_free (file->path);
  g_free (file->url);
  g_free (file->sha1);
  g_free (file);
}

NuvashaResolveFile *
nuvasha_resolve_file_new (const char *path, const char *url, const char *sha1,
                      gboolean natives, GPtrArray *exclude)
{
  NuvashaResolveFile *file = g_new0 (NuvashaResolveFile, 1);

  file->path = g_strdup (path);
  file->url = g_strdup (url);
  file->sha1 = g_strdup (sha1);
  file->natives = natives;
  file->exclude = exclude;

  return file;
}

/* Where the jar of @library belongs: its own maven path, which already
   carries the classifier of a natives jar. NULL when the coordinates
   make no path. */
static char *
library_file_path (const NuvashaLibrary *library, const char *classifier,
                   const char *libraries_dir)
{
  g_autofree char *from_coordinates = NULL;

  if (classifier != NULL)
    from_coordinates =
      nuvasha_resolve_maven_path_classified (library->name, classifier);
  else
    from_coordinates = nuvasha_resolve_maven_path (library->name);

  if (from_coordinates == NULL ||
      !nuvasha_resolve_entry_name_is_safe (from_coordinates))
    return NULL;

  return g_build_filename (libraries_dir, from_coordinates, NULL);
}

/* TRUE when a jar for @path is already on the list. Mojang repeats a
   library in a few documents and the same bytes must not be fetched
   twice nor unpacked twice. */
static gboolean
file_already_listed (GPtrArray *files, const char *path)
{
  for (guint i = 0; i < files->len; i++)
    {
      const NuvashaResolveFile *file = g_ptr_array_index (files, i);

      if (g_str_equal (file->path, path))
        return TRUE;
    }

  return FALSE;
}

/* Adds @info to @files when there is something to fetch and it is not
   there yet. A library that names no jar is skipped: the document does
   not say where it comes from, and that is not worth failing a whole
   version over. */
static void
add_file (GPtrArray *files, const NuvashaLibrary *library,
          const NuvashaDownloadInfo *info, const char *classifier,
          const char *libraries_dir, gboolean natives)
{
  g_autofree char *path = NULL;
  const char *published = info->path;

  if (info->url == NULL || info->url[0] == '\0')
    return;

  /* What the document published is where the jar belongs; the maven
     path of the coordinate is the same thing written out again. */
  if (published != NULL && nuvasha_resolve_entry_name_is_safe (published))
    path = g_build_filename (libraries_dir, published, NULL);
  else
    path = library_file_path (library, classifier, libraries_dir);

  if (path == NULL || file_already_listed (files, path))
    return;

  g_ptr_array_add (files, nuvasha_resolve_file_new (path, info->url, info->sha1,
                                                natives, library->exclude));
}

GPtrArray *
nuvasha_resolve_library_files (NuvashaVersionDescription *description,
                           const NuvashaRuleContext   *context,
                           const char             *libraries_dir)
{
  g_autoptr (GPtrArray) files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvasha_resolve_file_free);

  g_return_val_if_fail (description != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);
  g_return_val_if_fail (libraries_dir != NULL, NULL);

  for (guint i = 0; i < description->libraries->len; i++)
    {
      NuvashaLibrary *library = g_ptr_array_index (description->libraries, i);
      const char *classifier = NULL;

      if (!nuvasha_library_allowed (library, context))
        continue;

      /* The jar of the library goes on the classpath, whatever its
         coordinate says. Since 1.20.5 the natives are published as
         libraries of their own whose coordinate carries the classifier,
         and those jars belong on the classpath too: the document points
         LWJGL, JNA and netty at the directories where they unpack
         themselves out of the jars they find there. */
      if (library->artifact != NULL)
        add_file (files, library, library->artifact, NULL, libraries_dir,
                  FALSE);

      /* The older spelling: the library keeps its own jar in
         "downloads.artifact" and publishes the natives of this machine
         under a classifier of the "natives" table. Those are unpacked
         into java.library.path instead of being run from. */
      classifier = nuvasha_resolve_native_classifier (library, context);

      if (classifier != NULL)
        add_file (files, library,
                  g_hash_table_lookup (library->classifiers, classifier),
                  classifier, libraries_dir, TRUE);
    }

  return g_steal_pointer (&files);
}

char *
nuvasha_resolve_classpath (NuvashaVersionDescription *description,
                       const NuvashaRuleContext   *context,
                       const char             *libraries_dir,
                       const char             *client_jar)
{
  g_autoptr (GPtrArray) files = NULL;
  GString *classpath = g_string_new (NULL);
  gboolean first = TRUE;

  files = nuvasha_resolve_library_files (description, context, libraries_dir);

  for (guint i = 0; i < files->len; i++)
    {
      const NuvashaResolveFile *file = g_ptr_array_index (files, i);

      /* A natives jar is unpacked, not put on the classpath. */
      if (file->natives)
        continue;

      if (!first)
        g_string_append (classpath, CLASSPATH_SEPARATOR);

      g_string_append (classpath, file->path);
      first = FALSE;
    }

  /* Append the client JAR last. */
  if (client_jar != NULL)
    {
      if (!first)
        g_string_append (classpath, CLASSPATH_SEPARATOR);

      g_string_append (classpath, client_jar);
    }

  return g_string_free (classpath, FALSE);
}

/* ---------------------------------------------------------- *
 * Arguments
 * ---------------------------------------------------------- */

static GPtrArray *
arguments_of (NuvashaVersionDescription *description,
              const NuvashaRuleContext   *context, gboolean jvm)
{
  GPtrArray *arguments =
    jvm ? description->jvm_arguments : description->game_arguments;
  g_autoptr (GPtrArray) values = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < arguments->len; i++)
    {
      const NuvashaArgument *argument = g_ptr_array_index (arguments, i);

      if (!nuvasha_argument_allowed (argument, context))
        continue;

      for (guint j = 0; j < argument->values->len; j++)
        g_ptr_array_add (
            values, g_strdup (g_ptr_array_index (argument->values, j)));
    }

  return g_steal_pointer (&values);
}

GPtrArray *
nuvasha_resolve_jvm_arguments (NuvashaVersionDescription *description,
                           const NuvashaRuleContext   *context)
{
  g_return_val_if_fail (description != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);

  return arguments_of (description, context, TRUE);
}

GPtrArray *
nuvasha_resolve_game_arguments (NuvashaVersionDescription *description,
                            const NuvashaRuleContext   *context)
{
  g_return_val_if_fail (description != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);

  /* A version old enough to have one line instead of an array says so
     with the absence of the modern object. */
  if (!description->modern_arguments)
    return nuvasha_version_description_legacy_argv (description);

  return arguments_of (description, context, FALSE);
}
