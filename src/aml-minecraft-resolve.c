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

#include "aml-minecraft-resolve.h"
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
 * Small helpers
 * ---------------------------------------------------------- */

/* A version id or a file name from a document is only a file name after
   everything a file name cannot hold has been taken out of it. */
static char *
safe_name (const char *id)
{
  char *name = g_strdup (id);

  for (char *p = name; *p != '\0'; p++)
    {
      if (!g_ascii_isalnum (*p) && *p != '.' && *p != '-' && *p != '_')
        *p = '_';
    }

  return name;
}

static gboolean
ensure_directory (const char *path, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  if (!aml_fs_make_directory (path, 0755, &local_error))
    {
      g_set_error (error, AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_IO,
                   "cannot create \"%s\": %s", path,
                   local_error->message);
      return FALSE;
    }

  return TRUE;
}

/* Creates the directory @path lives in. */
static gboolean
ensure_parent_directory (const char *path, GError **error)
{
  g_autofree char *parent = g_path_get_dirname (path);

  return ensure_directory (parent, error);
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
aml_resolve_maven_path (const char *coordinate)
{
  return maven_path (coordinate, NULL);
}

char *
aml_resolve_maven_path_classified (const char *coordinate,
                                   const char *classifier)
{
  return maven_path (coordinate, classifier);
}

/* ---------------------------------------------------------- *
 * Choosing the natives of a library
 * ---------------------------------------------------------- */

/* The spellings of one architecture that are in use in the names of
   natives jars, the ones that are actually published first. */
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
aml_resolve_native_classifier (const AmlLibrary       *library,
                               const AmlRuleContext *context)
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
aml_resolve_entry_name_is_safe (const char *entry_name)
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
aml_resolve_file_free (AmlResolveFile *file)
{
  if (file == NULL)
    return;

  g_free (file->path);
  g_free (file->url);
  g_free (file->sha1);
  g_free (file);
}

static AmlResolveFile *
resolve_file_new (const char *path, const char *url, const char *sha1,
                  gboolean natives, GPtrArray *exclude)
{
  AmlResolveFile *file = g_new0 (AmlResolveFile, 1);

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
library_file_path (const AmlLibrary *library, const char *classifier,
                   const char *libraries_dir)
{
  g_autofree char *from_coordinates = NULL;

  if (classifier != NULL)
    from_coordinates =
      aml_resolve_maven_path_classified (library->name, classifier);
  else
    from_coordinates = aml_resolve_maven_path (library->name);

  if (from_coordinates == NULL ||
      !aml_resolve_entry_name_is_safe (from_coordinates))
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
      const AmlResolveFile *file = g_ptr_array_index (files, i);

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
add_file (GPtrArray *files, const AmlLibrary *library,
          const AmlDownloadInfo *info, const char *classifier,
          const char *libraries_dir, gboolean natives)
{
  g_autofree char *path = NULL;
  const char *published = info->path;

  if (info->url == NULL || info->url[0] == '\0')
    return;

  /* What the document published is where the jar belongs; the maven
     path of the coordinate is the same thing written out again. */
  if (published != NULL && aml_resolve_entry_name_is_safe (published))
    path = g_build_filename (libraries_dir, published, NULL);
  else
    path = library_file_path (library, classifier, libraries_dir);

  if (path == NULL || file_already_listed (files, path))
    return;

  g_ptr_array_add (files, resolve_file_new (path, info->url, info->sha1, natives,
                                           library->exclude));
}

GPtrArray *
aml_resolve_library_files (AmlVersionDescription *description,
                           const AmlRuleContext   *context,
                           const char             *libraries_dir)
{
  g_autoptr (GPtrArray) files =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_resolve_file_free);

  g_return_val_if_fail (description != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);
  g_return_val_if_fail (libraries_dir != NULL, NULL);

  for (guint i = 0; i < description->libraries->len; i++)
    {
      AmlLibrary *library = g_ptr_array_index (description->libraries, i);
      const char *classifier = NULL;

      if (!aml_library_allowed (library, context))
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
      classifier = aml_resolve_native_classifier (library, context);

      if (classifier != NULL)
        add_file (files, library,
                  g_hash_table_lookup (library->classifiers, classifier),
                  classifier, libraries_dir, TRUE);
    }

  return g_steal_pointer (&files);
}

char *
aml_resolve_classpath (AmlVersionDescription *description,
                       const AmlRuleContext   *context,
                       const char             *libraries_dir,
                       const char             *client_jar)
{
  g_autoptr (GPtrArray) files = NULL;
  GString *classpath = g_string_new (NULL);
  gboolean first = TRUE;

  files = aml_resolve_library_files (description, context, libraries_dir);

  for (guint i = 0; i < files->len; i++)
    {
      const AmlResolveFile *file = g_ptr_array_index (files, i);

      /* A natives jar is unpacked, not put on the classpath. */
      if (file->natives)
        continue;

      if (!first)
        g_string_append (classpath, CLASSPATH_SEPARATOR);

      g_string_append (classpath, file->path);
      first = FALSE;
    }

  /* The client jar goes last, so it is the one at the end. */
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
arguments_of (AmlVersionDescription *description,
              const AmlRuleContext   *context, gboolean jvm)
{
  GPtrArray *arguments =
    jvm ? description->jvm_arguments : description->game_arguments;
  g_autoptr (GPtrArray) values = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < arguments->len; i++)
    {
      const AmlArgument *argument = g_ptr_array_index (arguments, i);

      if (!aml_argument_allowed (argument, context))
        continue;

      for (guint j = 0; j < argument->values->len; j++)
        g_ptr_array_add (
            values, g_strdup (g_ptr_array_index (argument->values, j)));
    }

  return g_steal_pointer (&values);
}

GPtrArray *
aml_resolve_jvm_arguments (AmlVersionDescription *description,
                           const AmlRuleContext   *context)
{
  g_return_val_if_fail (description != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);

  return arguments_of (description, context, TRUE);
}

GPtrArray *
aml_resolve_game_arguments (AmlVersionDescription *description,
                            const AmlRuleContext   *context)
{
  g_return_val_if_fail (description != NULL, NULL);
  g_return_val_if_fail (context != NULL, NULL);

  /* A version old enough to have one line instead of an array says so
     with the absence of the modern object. */
  if (!description->modern_arguments)
    return aml_version_description_legacy_argv (description);

  return arguments_of (description, context, FALSE);
}

/* ---------------------------------------------------------- *
 * Unpacking the natives
 * ---------------------------------------------------------- */

/* TRUE when @name is @prefix, or lies below it when that is a
   directory. Mojang writes the prefixes with a trailing slash, which the
   directory entry of the jar itself does not have, so both spellings are
   tried. */
static gboolean
excluded_by (const char *prefix, const char *name)
{
  gsize length = strlen (prefix);

  if (g_str_has_prefix (name, prefix))
    return TRUE;

  return length > 1 && prefix[length - 1] == '/' &&
         strlen (name) == length - 1 &&
         strncmp (name, prefix, length - 1) == 0;
}

/* An entry is dropped when one of the exclude prefixes of its jar covers
   it. A jar whose document lists nothing is taken to exclude META-INF:
   no launcher wants the signatures and the metadata of a jar inside the
   natives the game loads. */
static gboolean
entry_excluded (GPtrArray *exclude, const char *name)
{
  if (exclude == NULL)
    return excluded_by ("META-INF/", name);

  for (guint i = 0; i < exclude->len; i++)
    {
      if (excluded_by (g_ptr_array_index (exclude, i), name))
        return TRUE;
    }

  return FALSE;
}

/* TRUE when @file is a directory and not something that only looks like
   one. A symlink is refused: an entry of a jar must never be able to
   make the next entry land outside the directory it is written to. */
static gboolean
is_plain_directory (GFile *file, GError **error)
{
  g_autoptr (GFileInfo) info = g_file_query_info (
      file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      NULL, error);

  if (info == NULL)
    return FALSE;

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
    {
      g_set_error (error, AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_UNSAFE_PATH,
                   "\"%s\" is not a directory", g_file_peek_path (file));
      return FALSE;
    }

  return TRUE;
}

/* TRUE when @file holds nothing yet, or something this module wrote
   before. A symlink where a regular file belongs is refused for the same
   reason a symlink is refused as a directory. */
static gboolean
is_writable_target (GFile *file, GError **error)
{
  g_autoptr (GFileInfo) info = g_file_query_info (
      file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      NULL, error);

  if (info == NULL)
    return TRUE; /* nothing is there: it can be created */

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
    {
      g_set_error (error, AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_UNSAFE_PATH,
                   "\"%s\" is not a regular file", g_file_peek_path (file));
      return FALSE;
    }

  return TRUE;
}

/* Creates every directory of @relative below @root, refusing anything
   that is not a plain directory on the way. */
static gboolean
make_parents (const char *root, const char *relative, GError **error)
{
  g_auto (GStrv) parts = NULL;
  g_autofree char *path = g_strdup (root);
  g_autoptr (GFile) child = NULL;

  parts = g_strsplit (relative, "/", -1);

  /* The last part is the file itself. */
  for (guint i = 0; parts[i + 1] != NULL; i++)
    {
      g_autofree char *next = g_build_filename (path, parts[i], NULL);
      g_autoptr (GError) local = NULL;

      g_free (path);
      path = g_steal_pointer (&next);

      g_clear_object (&child);
      child = g_file_new_for_path (path);

      if (g_file_query_exists (child, NULL))
        {
          if (!is_plain_directory (child, &local))
            {
              g_propagate_error (error, g_steal_pointer (&local));
              return FALSE;
            }

          continue;
        }

      if (!aml_fs_make_directory (path, 0755, &local))
        {
          g_propagate_error (error, g_steal_pointer (&local));
          return FALSE;
        }
    }

  return TRUE;
}

typedef struct
{
  GTask *task;
  AmlTaskScope *scope;
  GPtrArray *jars; /* AmlResolveFile*, a reference */
  char *directory;
  guint index;
} ExtractRequest;

/* One jar, open, while its entries are written one at a time. The bytes
   of the jar live as long as it does: libarchive reads out of them. */
typedef struct
{
  ExtractRequest *request;
  struct archive *archive;
  GBytes *bytes;
} ExtractStep;

static void
extract_request_free (ExtractRequest *request)
{
  aml_task_scope_unref (request->scope);
  g_clear_object (&request->task);
  g_clear_pointer (&request->jars, g_ptr_array_unref);
  g_free (request->directory);
  g_free (request);
}

static void
extract_step_free (ExtractStep *step)
{
  if (step->archive != NULL)
    archive_read_free (step->archive);

  g_clear_pointer (&step->bytes, g_bytes_unref);
  g_free (step);
}

static void extract_next_jar (ExtractRequest *request);
static void extract_entries (ExtractStep *step);

/* Ends the whole request. Consumes @error, which may be NULL. */
static void
extract_finished (ExtractRequest *request, GError *error)
{
  if (error != NULL)
    aml_task_scope_return_error (request->scope, error);
  else
    aml_task_scope_return_boolean (request->scope, TRUE);

  extract_request_free (request);
}

static void
extract_read_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;

  ExtractStep *step = user_data;
  AmlResolveFile *jar =
    g_ptr_array_index (step->request->jars, step->request->index);

  bytes = g_file_load_bytes_finish (G_FILE (source), result, NULL, &error);

  if (bytes == NULL)
    {
      extract_finished (step->request, g_steal_pointer (&error));
      extract_step_free (step);
      return;
    }

  /* The archive reads out of the bytes, so they stay until the jar is
     done with them. */
  step->bytes = g_steal_pointer (&bytes);
  step->archive = archive_read_new ();

  if (archive_read_support_format_zip (step->archive) != ARCHIVE_OK ||
      archive_read_open_memory (step->archive,
                                g_bytes_get_data (step->bytes, NULL),
                                g_bytes_get_size (step->bytes)) != ARCHIVE_OK)
    {
      extract_finished (
          step->request,
          g_error_new (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_INVALID,
                       "\"%s\" is not a jar AML can read: %s", jar->path,
                       archive_error_string (step->archive)));
      extract_step_free (step);
      return;
    }

  extract_entries (step);
}

/* One entry has been written: back into the jar for the next one. */
static void
extract_write_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  ExtractStep *step = user_data;

  aml_fs_replace_contents_finish (G_FILE (source), result, NULL, &error);

  if (error != NULL)
    {
      extract_finished (step->request, g_steal_pointer (&error));
      extract_step_free (step);
      return;
    }

  extract_entries (step);
}

/* Reads the whole body of the current entry into @error on failure. */
static gboolean
read_entry (struct archive *archive, GBytes **data, GError **error)
{
  g_autoptr (GByteArray) buffer = g_byte_array_new ();
  const void *block = NULL;
  gsize size = 0;
  la_int64_t offset = 0;
  int status;

  for (;;)
    {
      block = NULL;
      size = 0;
      offset = 0;

      status = archive_read_data_block (archive, &block, &size, &offset);

      if (status == ARCHIVE_EOF)
        break;

      if (status < ARCHIVE_WARN)
        {
          g_set_error (error, AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_INVALID,
                       "cannot read an entry of the jar: %s",
                       archive_error_string (archive));
          return FALSE;
        }

      if (size > 0)
        g_byte_array_append (buffer, block, size);
    }

  if (buffer->len == 0)
    {
      /* An empty entry is an empty file, not nothing at all. */
      g_byte_array_unref (g_steal_pointer (&buffer));
      *data = g_bytes_new_static ("", 0);
      return TRUE;
    }

  gsize length = buffer->len;
  *data = g_bytes_new_take (g_byte_array_free (g_steal_pointer (&buffer), FALSE),
                            length);

  return TRUE;
}

/* Walks the entries of the jar of @step and starts one write at a time,
   coming back here when it lands. */
static void
extract_entries (ExtractStep *step)
{
  ExtractRequest *request = step->request;
  AmlResolveFile *jar = g_ptr_array_index (request->jars, request->index);
  struct archive_entry *entry = NULL;

  while (archive_read_next_header (step->archive, &entry) == ARCHIVE_OK)
    {
      const char *name = archive_entry_pathname (entry);
      g_autofree char *relative = NULL;
      g_autofree char *path = NULL;
      g_autoptr (GBytes) data = NULL;
      g_autoptr (GFile) file = NULL;
      g_autoptr (GError) error = NULL;
      gsize length = 0;
      mode_t filetype = archive_entry_filetype (entry);

      if (name == NULL || entry_excluded (jar->exclude, name))
        continue;

      /* A directory entry ends in a slash, which is not a component of
         its own to test. */
      length = strlen (name);

      while (length > 0 && name[length - 1] == '/')
        length--;

      relative = g_strndup (name, length);

      if (!aml_resolve_entry_name_is_safe (relative))
        {
          extract_finished (
              request,
              g_error_new (AML_RESOLVE_ERROR, AML_RESOLVE_ERROR_UNSAFE_PATH,
                           "\"%s\" holds an entry that would be written "
                           "outside the natives directory: \"%s\"",
                           jar->path, name));
          extract_step_free (step);
          return;
        }

      path = g_build_filename (request->directory, relative, NULL);

      if (filetype == AE_IFDIR)
        {
          if (!ensure_directory (path, &error))
            {
              extract_finished (request, g_steal_pointer (&error));
              extract_step_free (step);
              return;
            }

          continue;
        }

      /* Only regular files are written. A symlink, a hard link or a
         device inside a jar is skipped: honouring one would let a jar
         put a name of the natives directory somewhere else on the
         disk, or run something that was never meant to be run. */
      if (filetype != AE_IFREG)
        continue;

      if (!make_parents (request->directory, relative, &error))
        {
          extract_finished (request, g_steal_pointer (&error));
          extract_step_free (step);
          return;
        }

      file = g_file_new_for_path (path);

      if (!is_writable_target (file, &error))
        {
          extract_finished (request, g_steal_pointer (&error));
          extract_step_free (step);
          return;
        }

      if (!read_entry (step->archive, &data, &error))
        {
          extract_finished (request, g_steal_pointer (&error));
          extract_step_free (step);
          return;
        }

       aml_fs_replace_contents_bytes_async (file, data, NULL, FALSE,
                                            G_FILE_CREATE_NONE,
                                            g_task_get_cancellable (
                                                request->task),
                                            extract_write_ready, step);
      return;
    }

  /* The jar is done: on to the next one. */
  request->index++;
  extract_step_free (step);
  extract_next_jar (request);
}

static void
extract_next_jar (ExtractRequest *request)
{
  AmlResolveFile *jar;
  ExtractStep *step;
  g_autoptr (GFile) file = NULL;

  if (request->index >= request->jars->len)
    {
      extract_finished (request, NULL);
      return;
    }

  jar = g_ptr_array_index (request->jars, request->index);
  file = g_file_new_for_path (jar->path);

  step = g_new0 (ExtractStep, 1);
  step->request = request;

  g_file_load_bytes_async (file, g_task_get_cancellable (request->task),
                           extract_read_ready, step);
}

AmlTaskScope *
aml_resolve_extract_natives_async (GPtrArray         *jars,
                                   const char        *natives_directory,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  ExtractRequest *request;

  g_return_val_if_fail (jars != NULL, NULL);
  g_return_val_if_fail (natives_directory != NULL, NULL);
  g_return_val_if_fail (g_path_is_absolute (natives_directory), NULL);

  request = g_new0 (ExtractRequest, 1);

  /* Keep a reference separate from the handle returned to the caller. The
     completion callback may release that handle before this request has
     finished cleaning up its task state. */
  request->task = g_object_ref (aml_task_scope_task (scope));
  request->scope = aml_task_scope_ref (scope);
  g_task_set_check_cancellable (request->task, FALSE);
  request->jars = g_ptr_array_ref (jars);
  request->directory = g_strdup (natives_directory);

  extract_next_jar (request);

  return g_steal_pointer (&scope);
}

gboolean
aml_resolve_extract_natives_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
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
                         resolve_file_new (file->path, file->url, file->sha1,
                                           TRUE, file->exclude));
    }

  if (!ensure_parent_directory (request->client_jar, &error))
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
      if (!ensure_parent_directory (request->logging_path, &error))
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

      if (!ensure_parent_directory (file->path, &error))
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

  if (!ensure_directory (request->natives_dir, &error))
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

      if (!ensure_parent_directory (target, &error))
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

  if (!aml_fs_file_size_within (path, AML_FS_MAX_ASSET_INDEX_SIZE, &error) ||
      !g_file_get_contents (path, &text, &length, &error) ||
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

      if (!ensure_parent_directory (destination, &local))
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

  if (!ensure_parent_directory (path, &error))
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
      g_autofree char *logging_name = safe_name (
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
  request->asset_index_name = g_strdup (request->description->assets);

  g_free (request->asset_index_id);

  if (request->description->asset_index != NULL &&
      request->description->asset_index->id != NULL)
    {
      g_autofree char *safe =
        safe_name (request->description->asset_index->id);

      request->asset_index_id = g_strconcat (safe, ".json", NULL);
    }
  else
    {
      g_autofree char *safe =
        safe_name (request->description->assets != NULL
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
  g_autofree char *name = safe_name (version_id);
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

  if (!ensure_directory (request->libraries_dir, &error) ||
      !ensure_directory (request->assets_dir, &error) ||
      !ensure_directory (request->versions_dir, &error))
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
