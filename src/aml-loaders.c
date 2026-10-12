/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-loaders.h"
#include "aml-loaders-private.h"
#include "aml-task.h"
#include "aml-fs.h"

#include "aml-download.h"
#include "aml-minecraft-resolve.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INSTALLER_ENTRY_SIZE (128ULL * 1024ULL * 1024ULL)

GQuark
aml_loaders_error_quark (void)
{
  return g_quark_from_static_string ("aml-loaders-error");
}

/* ---------------------------------------------------------- *
 * The installer of a loader that publishes one
 * ---------------------------------------------------------- */

/* NeoForge and Forge publish no version document to fetch: they publish a
 * jar, the installer, which carries a profile saying which processors turn
 * the client jar of a Minecraft version into the client jar of the loader,
 * and the document the game is then run with.
 *
 * An install is that profile followed one processor at a time, with a Java
 * to run them with and no window anywhere: every processor is a tool that
 * reads files and writes files, which is why the GUI of their own
 * installers is not needed here. What the processors produce is kept, so
 * the next launch of the same instance has nothing to do. */

void
aml_loader_install_free (AmlLoaderInstall *install)
{
  if (install == NULL)
    return;

  g_clear_object (&install->description);
  g_free (install->client_jar);
  g_free (install);
}

static gboolean
ensure_directory (const char *path, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  if (aml_fs_make_directory (path, 0755, &local_error))
    return TRUE;

  g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
               "the directory \"%s\" cannot be made: %s", path,
               local_error->message);

  return FALSE;
}

/* Writes an extracted installer file beside its destination and publishes
 * it only after the complete byte stream is durable. A failed processor or
 * interrupted process therefore cannot leave a half-written library. */
static gboolean
write_install_file (const char *path, const void *data, gsize size,
                    GError **error)
{
  g_autofree char *directory = g_path_get_dirname (path);
  g_autofree char *temporary = NULL;
  g_autoptr (GError) local_error = NULL;
  int fd = aml_fs_open_temporary_file (directory, ".aml-loader-XXXXXX",
                                       &temporary, error);

  if (fd < 0)
    {
      return FALSE;
    }
  close (fd);

  if (!aml_fs_replace_contents (temporary, data, (gssize) size, 0600,
                                &local_error) ||
      !aml_fs_rename (temporary, path, &local_error))
    {
      if (local_error == NULL)
        g_set_error (&local_error, AML_LOADERS_ERROR,
                     AML_LOADERS_ERROR_INSTALLER,
                     "cannot publish \"%s\": %s", path, g_strerror (errno));
      aml_fs_unlink (temporary, NULL);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_chmod (path, 0644);
  return TRUE;
}

/* The bytes of @entry inside the zip archive at @path, which is what an
 * installer is: a jar holding its profile, the document of the loader and
 * the patches it applies. */
static GBytes *
read_zip_entry (const char *path, const char *entry, GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  GByteArray *bytes = NULL;
  g_autofree char *archive_message = NULL;
  int status = ARCHIVE_EOF;

  archive = archive_read_new ();

  if (archive_read_support_format_zip (archive) != ARCHIVE_OK ||
      archive_read_open_filename (archive, path, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the installer at \"%s\" cannot be read: %s", path,
                   archive_error_string (archive));
      archive_read_free (archive);
      return NULL;
    }

  while ((status = archive_read_next_header (archive, &header)) == ARCHIVE_OK)
    {
      const char *name = NULL;

      if (archive_entry_filetype (header) != AE_IFREG)
        continue;

      name = archive_entry_pathname (header);

      if (name == NULL || !g_str_equal (name, entry))
        continue;

      if (archive_entry_size_is_set (header) &&
          archive_entry_size (header) > (la_int64_t) MAX_INSTALLER_ENTRY_SIZE)
        {
          g_set_error (error, AML_LOADERS_ERROR,
                       AML_LOADERS_ERROR_INSTALLER,
                       "the \"%s\" of the installer is too large", entry);
          break;
        }

      bytes = g_byte_array_new ();

      for (;;)
        {
          const void *block = NULL;
          size_t size = 0;
          la_int64_t offset = 0;
          int status =
            archive_read_data_block (archive, &block, &size, &offset);

          if (status == ARCHIVE_EOF)
            break;

          if (status != ARCHIVE_OK)
            {
              g_set_error (error, AML_LOADERS_ERROR,
                           AML_LOADERS_ERROR_INSTALLER,
                           "the \"%s\" of the installer cannot be read: %s",
                           entry, archive_error_string (archive));
              g_byte_array_unref (bytes);
              bytes = NULL;
              break;
            }

          if ((guint64) bytes->len + size > MAX_INSTALLER_ENTRY_SIZE)
            {
              g_set_error (error, AML_LOADERS_ERROR,
                           AML_LOADERS_ERROR_INSTALLER,
                           "the \"%s\" of the installer is too large", entry);
              g_byte_array_unref (bytes);
              bytes = NULL;
              break;
            }

          g_byte_array_append (bytes, block, size);
        }

      break;
    }

  if (status != ARCHIVE_EOF)
    archive_message = g_strdup (archive_error_string (archive));

  archive_read_close (archive);
  archive_read_free (archive);

  if (bytes == NULL && error != NULL && *error == NULL)
    {
      if (status != ARCHIVE_EOF)
        g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                     "the installer at \"%s\" cannot be read: %s", path,
                     archive_message != NULL ? archive_message : "unknown error");
      else
        g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                     "the installer at \"%s\" has no \"%s\"", path, entry);
    }

  if (bytes == NULL)
    return NULL;

  return g_byte_array_free_to_bytes (bytes);
}

/* What the manifest of the jar at @path says about @name, or NULL. */
static char *
jar_manifest_attribute (const char *path, const char *name)
{
  g_autoptr (GBytes) manifest = NULL;
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  const char *data = NULL;
  gsize size = 0;
  g_autofree char *prefix = NULL;

  manifest = read_zip_entry (path, "META-INF/MANIFEST.MF", NULL);

  if (manifest == NULL)
    return NULL;

  data = g_bytes_get_data (manifest, &size);
  text = g_strndup (data, size);
  lines = g_strsplit (text, "\n", -1);
  prefix = g_strconcat (name, ":", NULL);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      const char *line = g_strstrip (lines[i]);

      if (*line == ' ' || *line == '\0')
        continue;

      if (g_str_has_prefix (line, prefix))
        return g_strstrip (g_strdup (line + strlen (prefix)));
    }

  return NULL;
}

/* Writes @value for @name into the manifest of the jar at @path, writing
 * the jar out again where it is: a manifest is a file of its own inside
 * the jar and cannot be changed in place.
 *
 * This is how the patched client jar of a loader says which distribution
 * it is. The loader of the game reads that attribute before it loads
 * anything, and refuses to start with "the installation is corrupted"
 * when the jar says nothing. */
static gboolean
write_jar_manifest_attribute (const char *path, const char *name,
                              const char *value, GError **error)
{
  struct archive *in = NULL;
  struct archive *out = NULL;
  struct archive_entry *header = NULL;
  g_autofree char *temporary = NULL;
  gboolean written = FALSE;
  gboolean ok = TRUE;
  g_autoptr (GError) local_error = NULL;
  int status = ARCHIVE_EOF;

  in = archive_read_new ();

  if (archive_read_support_format_zip (in) != ARCHIVE_OK ||
      archive_read_open_filename (in, path, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the patched client jar at \"%s\" cannot be read: %s", path,
                   archive_error_string (in));
      archive_read_free (in);
      return FALSE;
    }

  /* The jar is written out beside itself and moved over it only once it
     is whole, the same way a download is: a jar that is half written is a
     broken game. */
  temporary = g_strconcat (path, ".aml-new", NULL);

  out = archive_write_new ();
  archive_write_set_format_zip (out);

  if (archive_write_open_filename (out, temporary) != ARCHIVE_OK)
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the patched client jar at \"%s\" cannot be written: %s",
                   path, archive_error_string (out));
      archive_read_free (in);
      archive_write_free (out);
      aml_fs_unlink (temporary, NULL);
      return FALSE;
    }

  while ((status = archive_read_next_header (in, &header)) == ARCHIVE_OK)
    {
      struct archive_entry *copy = archive_entry_clone (header);
      g_autoptr (GBytes) bytes = NULL;
      g_autofree char *text = NULL;
      const void *block = NULL;
      size_t size = 0;
      la_int64_t offset = 0;
      int status;

      if (archive_write_header (out, copy) != ARCHIVE_OK)
        {
          archive_entry_free (copy);
          ok = FALSE;
          break;
        }

      if (archive_entry_pathname (header) == NULL)
        {
          archive_entry_free (copy);
          ok = FALSE;
          break;
        }

      if (g_str_equal (archive_entry_pathname (header), "META-INF/MANIFEST.MF"))
        {
          g_autofree char *attribute = NULL;

          bytes = read_zip_entry (path, "META-INF/MANIFEST.MF", NULL);

          if (bytes != NULL)
            {
              text = g_strndup (g_bytes_get_data (bytes, NULL),
                                g_bytes_get_size (bytes));

              /* The main attributes are what comes before the first empty
                 line of a manifest; the entries of the jar follow it. */
              attribute = g_strconcat (name, ": ", value, "\r\n", NULL);

              {
                const char *split = strstr (text, "\r\n\r\n");
                g_autofree char *head = g_strndup (text, split != NULL
                                                    ? (gsize) (split - text)
                                                    : strlen (text));
                g_autofree char *with_attribute =
                  g_strconcat (head, "\r\n", attribute,
                               split != NULL ? split : "", NULL);

                g_free (text);
                text = g_steal_pointer (&with_attribute);
              }

              archive_entry_set_size (copy, strlen (text));
              written = archive_write_data (out, text, strlen (text)) ==
                        (la_ssize_t) strlen (text);
              if (!written)
                ok = FALSE;
            }
        }
      else
        {
          for (;;)
            {
              status = archive_read_data_block (in, &block, &size, &offset);

              if (status != ARCHIVE_OK)
                break;

              if (archive_write_data (out, block, size) != (la_ssize_t) size)
                {
                  ok = FALSE;
                  break;
                }
            }
        }

      archive_entry_free (copy);

      if (!ok)
        break;
    }

  if (status != ARCHIVE_EOF)
    ok = FALSE;

  archive_read_close (in);
  archive_read_free (in);

  if (!ok || archive_write_close (out) != ARCHIVE_OK)
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the patched client jar at \"%s\" cannot be written: %s",
                   path, out != NULL ? archive_error_string (out) : "");
      archive_write_free (out);

      aml_fs_unlink (temporary, NULL);
      return FALSE;
    }

  archive_write_free (out);

  if (!written)
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the patched client jar at \"%s\" has no manifest to "
                   "write %s into", path, name);
      aml_fs_unlink (temporary, NULL);
      return FALSE;
    }

  if (!aml_fs_rename (temporary, path, &local_error))
    {
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the patched client jar at \"%s\" cannot be replaced: %s",
                    path, local_error->message);
      aml_fs_unlink (temporary, NULL);
      return FALSE;
    }

  return TRUE;
}

/* The class a jar names as its main, which is how a processor says how it
 * is to be run: an install profile names the jar and never the class. */
static char *
jar_main_class (const char *path, GError **error)
{
  g_autoptr (GBytes) manifest = NULL;
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  const char *data = NULL;
  gsize size = 0;

  manifest = read_zip_entry (path, "META-INF/MANIFEST.MF", error);

  if (manifest == NULL)
    return NULL;

  /* A manifest is a file and not a string: it ends where its bytes end,
     so the text of it is made before it is read line by line. */
  data = g_bytes_get_data (manifest, &size);
  text = g_strndup (data, size);
  lines = g_strsplit (text, "\n", -1);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      const char *line = g_strstrip (lines[i]);

      /* A line that starts with a space continues the one before it,
         which a long class name can need. */
      if (*line == ' ' || *line == '\0')
        continue;

      if (g_str_has_prefix (line, "Main-Class:"))
        return g_strstrip (g_strdup (line + strlen ("Main-Class:")));
    }

  g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
               "the jar \"%s\" says no class to run it with", path);

  return NULL;
}

/* One file the install needs before it can go on. */
typedef enum {
  FILE_FETCH,  /* out of the maven the loader publishes */
  FILE_UNPACK  /* out of the installer jar */
} InstallFileKind;

typedef struct {
  InstallFileKind kind;
  char *url;   /* FILE_FETCH: where it comes from */
  char *entry; /* FILE_UNPACK: what it is called inside the installer */
  char *path;  /* where it lands either way */
  char *sha1;  /* what it should hash to, when anything says */

  /* TRUE for an artifact the maven does not have, which is what a name
     in the data of a profile usually is: the place a processor writes
     its output to, which only the processors know. A maven that answers
     404 about one of these says nothing wrong, so the install goes on
     without it; a tool that did need it says so itself, and what it
     printed comes with the error. */
  gboolean optional;
} InstallFile;

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

/* What an install is on, which is one step per thing it does. */
typedef enum {
  STAGE_CLIENT_JAR, /* the client jar the processors patch */
  STAGE_INSTALLER,  /* the installer jar itself */
  STAGE_PROFILE,    /* the profile and the document out of it */
  STAGE_VARIABLES,  /* what one processor needs to be there */
  STAGE_RUN,        /* one processor running */
  STAGE_DONE
} InstallStage;

typedef struct {
  GTask *task;
  AmlTaskScope *scope;

  AmlLoader loader;
  InstallStage stage;

  char *data_dir;        /* where the games are kept */
  char *game_version;
  char *loader_version;
  char *java;            /* the java that runs the processors */
  char *root;            /* where this loader version is installed */
  char *libraries_dir;   /* where the artifacts of the profile go */
  char *installer;       /* the installer jar of this version */
  char *client_jar;      /* the client jar of the game version */
  char *patched_jar;     /* what the processors made of it */

  /* The install profile, read out of the installer jar. What is kept is
     the parser that owns it: a parsed document is not counted, so
     everything asked of it has to be asked of the parser. */
  JsonParser *profile;
  AmlVersionDescription *description; /* the document of the loader */
  GPtrArray *processors; /* JsonNode*, the ones the client side runs */
  guint index;           /* which of them is being run */
  char *main_class;      /* of the one being run */

  /* What the names in the arguments of a processor stand for, filled for
     one processor at a time. */
  GHashTable *variables;

  GPtrArray *pending; /* InstallFile*, fetched one after the other */
  guint done;

  AmlLoadersProgressFunc progress;
  gpointer progress_data;
} InstallRequest;

static void
install_request_free (gpointer data)
{
  InstallRequest *request = data;

  aml_task_scope_unref (request->scope);
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

/* Ends the install. Consumes @error, which may be NULL. */
static void
install_finished (InstallRequest *request, GError *error)
{
  if (error != NULL)
    {
      g_task_return_error (request->task, error);
    }
  else
    {
      AmlLoaderInstall *install = g_new0 (AmlLoaderInstall, 1);

      install->description = g_object_ref (request->description);
      install->client_jar = g_strdup (request->patched_jar);

      g_task_return_pointer (request->task, install,
                             (GDestroyNotify) aml_loader_install_free);
    }

  install_request_free (request);
}

static void
report_progress (InstallRequest *request)
{
  if (request->progress == NULL)
    return;

  request->progress ("installing the loader", request->done,
                     request->done + request->pending->len,
                     request->progress_data);
}

static void
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
static char *
artifact_path (InstallRequest *request, const char *coordinate)
{
  g_autofree char *relative = NULL;

  relative = aml_resolve_maven_path (coordinate);

  if (relative == NULL || !aml_resolve_entry_name_is_safe (relative))
    return NULL;

  return g_build_filename (request->libraries_dir, relative, NULL);
}

/* The artifact @coordinate names out of the maven of the loader. What a
 * previous install fetched is used as it is: nothing beside an artifact of
 * a loader maven says what it should hash to, so the loader's own
 * repository over https is what it rests on.
 *
 * @optional is TRUE for the names the profile takes out of its data,
 * which name where the processors write as much as what they read. */
static void
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

  relative = aml_resolve_maven_path (coordinate);

  if (relative == NULL || !aml_resolve_entry_name_is_safe (relative))
    return;

  path = g_build_filename (request->libraries_dir, relative, NULL);

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return;

  url = g_strconcat (maven, "/", relative, NULL);
  queue_file (request, FILE_FETCH, url, NULL, path, NULL, optional);
}

/* The file @entry of the installer jar, such as the patch the processors
 * apply, written out where the processors can find it. */
static void
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
      !aml_resolve_entry_name_is_safe (entry))
    return;

  path = g_build_filename (request->root, entry, NULL);

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return;

  queue_file (request, FILE_UNPACK, NULL, entry, path, NULL, FALSE);
}

static void drain_pending (InstallRequest *request);
static void install_step_done (InstallRequest *request);

/* The last words of what a processor printed, which is what says why it
 * failed: a tool that stops says nothing useful on its own. */
static char *
what_a_processor_said (const char *output, guint count)
{
  g_auto (GStrv) lines = NULL;
  g_autoptr (GString) kept = NULL;
  guint saying = 0;

  if (output == NULL || *output == '\0')
    return NULL;

  lines = g_strsplit (output, "\n", -1);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      if (*g_strstrip (lines[i]) != '\0')
        saying++;
    }

  if (saying == 0)
    return NULL;

  /* The last @count lines that say something: an error of a tool is in
     the last of them, not in the first. */
  kept = g_string_new (NULL);
  saying -= MIN (saying, count);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      const char *line = g_strstrip (lines[i]);

      if (*line == '\0')
        continue;

      if (saying > 0)
        {
          saying--;
          continue;
        }

      if (kept->len > 0)
        g_string_append_c (kept, '\n');

      g_string_append (kept, line);
    }

  return g_strdup (kept->str);
}

static void
processor_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  InstallRequest *request = user_data;
  GSubprocess *process = G_SUBPROCESS (source);
  g_autoptr (GError) error = NULL;
  g_autofree char *output = NULL;
  g_autofree char *said = NULL;

  if (!g_subprocess_communicate_utf8_finish (process, result, &output, NULL,
                                             &error))
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  if (g_subprocess_get_successful (process))
    {
      report_progress (request);
      install_step_done (request);
      return;
    }

  /* The tool says why it stopped in what it printed, and the exit status
     alone says nothing. */
  said = what_a_processor_said (output, 12);

  g_set_error (&error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
               "the processor that patches the game for %s %s failed with "
               "status %d%s%s", aml_loader_display_name (request->loader),
               request->loader_version, g_subprocess_get_exit_status (process),
               said != NULL ? ": " : "", said != NULL ? said : "");

  install_failed (request, g_steal_pointer (&error));
}

static void
file_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
            gpointer user_data)
{
  InstallRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  InstallFile *file = g_ptr_array_index (request->pending, 0);

  if (!aml_download_file_finish (result, &error))
    {
      /* A maven that does not hold an artifact a name of the profile says
         nothing about: only the processors know, and they are run next. */
      if (file->optional && g_error_matches (error, AML_DOWNLOAD_ERROR,
                                             AML_DOWNLOAD_ERROR_HTTP))
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

static void
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

  if (!ensure_directory (directory, &error))
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  if (file->kind == FILE_UNPACK)
    {
      g_autoptr (GBytes) bytes =
        read_zip_entry (request->installer, file->entry, &error);
      const char *data = NULL;
      gsize size = 0;

      if (bytes == NULL)
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }

      data = g_bytes_get_data (bytes, &size);

      if (!write_install_file (file->path, data, size, &error))
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

  aml_download_file_async (file->url, file->path, file->sha1,
                           file->sha1 != NULL ? G_CHECKSUM_SHA1
                                              : G_CHECKSUM_MD5,
                           g_task_get_cancellable (request->task), NULL,
                           file_ready, request);
}

/* ---------------------------------------------------------- *
 * What the names in the arguments of a processor stand for
 * ---------------------------------------------------------- */

/* The profile of the install, as the parser that read it keeps it. */
static JsonObject *
install_profile (InstallRequest *request)
{
  JsonNode *root = NULL;

  if (request->profile == NULL)
    return NULL;

  root = json_parser_get_root (request->profile);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;

  return json_node_get_object (root);
}

/* What the profile says about one name on the client side: either the
 * value is the same on every side, or each side names its own. */
static const char *
profile_data (InstallRequest *request, const char *name)
{
  JsonNode *data = NULL;
  JsonNode *entry = NULL;
  JsonObject *profile = install_profile (request);

  if (profile == NULL)
    return NULL;

  data = json_object_get_member (profile, "data");

  if (data == NULL || !JSON_NODE_HOLDS_OBJECT (data))
    return NULL;

  entry = json_object_get_member (json_node_get_object (data), name);

  if (entry == NULL)
    return NULL;

  if (JSON_NODE_HOLDS_VALUE (entry) &&
      json_node_get_value_type (entry) == G_TYPE_STRING)
    return json_node_get_string (entry);

  if (!JSON_NODE_HOLDS_OBJECT (entry))
    return NULL;

  entry = json_object_get_member (json_node_get_object (entry), "client");

  if (entry != NULL && JSON_NODE_HOLDS_VALUE (entry) &&
      json_node_get_value_type (entry) == G_TYPE_STRING)
    return json_node_get_string (entry);

  return NULL;
}

/* The names the installer gives to the files of an install itself: where
 * it is installed, the jar it was run from, the client jar its processors
 * patch, the side they run for and the version they are for. */
static const char *
installer_data (InstallRequest *request, const char *name)
{
  /* The root of an install is where its artifacts are, which is the
     directory the games keep their libraries in: that is where the
     documents of the loaders say to find them. */
  if (g_str_equal (name, "ROOT"))
    return request->data_dir;

  if (g_str_equal (name, "INSTALLER"))
    return request->installer;

  if (g_str_equal (name, "MINECRAFT_JAR"))
    return request->client_jar;

  if (g_str_equal (name, "SIDE"))
    return "client";

  if (g_str_equal (name, "VERSION_NAME"))
    return request->game_version;

  if (g_str_equal (name, "CACHE_DIR"))
    return request->root;

  return NULL;
}

static gboolean
resolve_variable (InstallRequest *request, const char *name, GError **error)
{
  const char *value = NULL;
  gsize length = 0;

  if (g_hash_table_contains (request->variables, name))
    return TRUE;

  value = profile_data (request, name);

  if (value == NULL)
    {
      /* The names the installer itself gives are the paths it works on,
         not values of the profile: they are taken as they are. */
      value = installer_data (request, name);

      if (value == NULL)
        {
          g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                       "the install profile of %s %s says nothing about "
                       "{%s}, which its processors ask for",
                       aml_loader_display_name (request->loader),
                       request->loader_version, name);
          return FALSE;
        }

      g_hash_table_insert (request->variables, g_strdup (name),
                           g_strdup (value));
      return TRUE;
    }

  length = strlen (value);

  /* An artifact of the maven of the loader, written as "[coordinate]". */
  if (length > 2 && value[0] == '[' && value[length - 1] == ']')
    {
      g_autofree char *coordinate = g_strndup (value + 1, length - 2);
      g_autofree char *path = artifact_path (request, coordinate);

      if (path == NULL)
        {
          g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_MAVEN,
                       "\"%s\" is no path an artifact of %s can live at",
                       coordinate, aml_loader_display_name (request->loader));
          return FALSE;
        }

      queue_artifact (request, coordinate, TRUE);

      if (g_str_equal (name, "PATCHED"))
        {
          g_free (request->patched_jar);
          request->patched_jar = g_strdup (path);
        }

      g_hash_table_insert (request->variables, g_strdup (name),
                           g_steal_pointer (&path));
      return TRUE;
    }

  /* A file inside the installer jar, written as a path in it. */
  if (value[0] == '/')
    {
      g_autofree char *path = NULL;

      queue_unpacked (request, value);
      path = g_build_filename (request->root, value + 1, NULL);

      g_hash_table_insert (request->variables, g_strdup (name),
                           g_steal_pointer (&path));
      return TRUE;
    }

  /* Anything else is a value the profile gives as it is: a mapping name, a
     version, a date. */
  g_hash_table_insert (request->variables, g_strdup (name), g_strdup (value));

  return TRUE;
}

/* One argument of a processor, with every "{NAME}" in it replaced by what
 * the name stands for. */
static char *
substitute_argument (InstallRequest *request, const char *argument,
                     GError **error)
{
  g_autoptr (GString) result = g_string_new (NULL);

  for (const char *p = argument; *p != '\0'; p++)
    {
      const char *end = NULL;
      g_autofree char *name = NULL;
      const char *value = NULL;

      if (*p != '{')
        {
          g_string_append_c (result, *p);
          continue;
        }

      end = strchr (p, '}');

      /* A brace the profile did not open is a brace. */
      if (end == NULL)
        {
          g_string_append_c (result, *p);
          continue;
        }

      name = g_strndup (p + 1, end - p - 1);

      if (!resolve_variable (request, name, error))
        return NULL;

      value = g_hash_table_lookup (request->variables, name);
      g_string_append (result, value != NULL ? value : name);

      p = end;
    }

  return g_strdup (result->str);
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
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_UNSUPPORTED,
                                   "AML does not install %s",
                                   aml_loader_display_name (request->loader)));
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
static AmlVersionDescription *
document_with_install_libraries (InstallRequest *request, const char *json,
                                 gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (JsonGenerator) generator = NULL;
  g_autoptr (AmlVersionDescription) document = NULL;
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
      g_set_error (error, AML_LOADERS_ERROR, AML_LOADERS_ERROR_INSTALLER,
                   "the version document of %s %s is not a document AML can "
                   "read", aml_loader_display_name (request->loader),
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

  document = aml_version_description_new_from_json (data, written, error);

  return g_steal_pointer (&document);
}

/* The install profile out of the installer jar, and the document the
 * loader runs the game with, which is in the same jar. */
static void
stage_profile (InstallRequest *request)
{
  g_autoptr (GBytes) profile_bytes = NULL;
  g_autoptr (GBytes) document_bytes = NULL;
  g_autoptr (AmlVersionDescription) description = NULL;
  g_autoptr (GError) error = NULL;
  JsonParser *parser = NULL;
  const char *json_entry = "version.json";
  JsonNode *node = NULL;

  request->stage = STAGE_PROFILE;

  profile_bytes =
    read_zip_entry (request->installer, "install_profile.json", &error);

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
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_INSTALLER,
                                   "the install profile of %s %s is not a "
                                   "document AML can read",
                                   aml_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  /* The parser is what keeps the profile, and the processors below are
     parts of it. */
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
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_GENERATION,
                                   "the install profile of %s %s runs no "
                                   "processor for the client, which is not a "
                                   "shape AML knows how to install",
                                   aml_loader_display_name (request->loader),
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

  document_bytes = read_zip_entry (request->installer, json_entry, &error);

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

/* Everything one processor needs to be there before it runs: what the
 * names in its arguments stand for, and the jars it runs with. */
static void
stage_processor (InstallRequest *request)
{
  JsonNode *entry = NULL;
  JsonObject *processor = NULL;
  JsonNode *node = NULL;
  g_autoptr (GError) error = NULL;

  request->stage = STAGE_VARIABLES;

  if (request->index >= request->processors->len)
    {
      install_failed (request,
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_INSTALLER,
                                   "the install of %s %s has no processor to "
                                   "run",
                                   aml_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  entry = g_ptr_array_index (request->processors, request->index);
  processor = json_node_get_object (entry);

  /* Each processor gets names of its own: two of them may well use the
     same name for different things. */
  g_clear_pointer (&request->variables, g_hash_table_unref);
  request->variables = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              g_free);
  g_clear_pointer (&request->main_class, g_free);

  /* The arguments first: what they name is what has to be there. */
  node = json_object_get_member (processor, "args");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          g_autofree char *argument = NULL;

          if (!JSON_NODE_HOLDS_VALUE (element) ||
              json_node_get_value_type (element) != G_TYPE_STRING)
            continue;

          argument = substitute_argument (
            request, json_node_get_string (element), &error);

          if (argument == NULL)
            {
              install_failed (request, g_steal_pointer (&error));
              return;
            }
        }
    }

  /* Then the jars it runs with: the tools of the processor, and the jar of
     the processor itself, which is where the class to run is read from. */
  node = json_object_get_member (processor, "classpath");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);

          if (JSON_NODE_HOLDS_VALUE (element) &&
              json_node_get_value_type (element) == G_TYPE_STRING)
            queue_artifact (request, json_node_get_string (element), FALSE);
        }
    }

  node = json_object_get_member (processor, "jar");

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    queue_artifact (request, json_node_get_string (node), FALSE);

  drain_pending (request);
}

/* One processor, run the way its profile says: the java that runs the game
 * runs this too, with the class the jar of the processor names. */
static void
stage_run (InstallRequest *request)
{
  JsonObject *processor =
    json_node_get_object (g_ptr_array_index (request->processors,
                                             request->index));
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GString) classpath = NULL;
  g_autoptr (GError) error = NULL;
  JsonNode *node = NULL;
  const char *tool = NULL;

  request->stage = STAGE_RUN;

  node = json_object_get_member (processor, "jar");

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      install_failed (request,
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_INSTALLER,
                                   "a processor of %s %s names no jar to run",
                                   aml_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  tool = json_node_get_string (node);

  {
    g_autofree char *jar = artifact_path (request, tool);

    if (jar == NULL)
      {
        install_failed (
          request,
          g_error_new (AML_LOADERS_ERROR, AML_LOADERS_ERROR_NO_MAVEN,
                       "\"%s\" is no path a processor of %s can live at", tool,
                       aml_loader_display_name (request->loader)));
        return;
      }

    request->main_class = jar_main_class (jar, &error);

    if (request->main_class == NULL)
      {
        install_failed (request, g_steal_pointer (&error));
        return;
      }
  }

  /* The class path of a processor is the jar of the processor and the jars
     it runs with: NeoForge names its tool among them and Forge does not,
     and a tool that is not on its own class path has no class to run. */
  classpath = g_string_new (NULL);

  {
    g_autofree char *own = artifact_path (request, tool);

    if (own != NULL)
      g_string_append (classpath, own);
  }

  node = json_object_get_member (processor, "classpath");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          g_autofree char *path = NULL;

          if (!JSON_NODE_HOLDS_VALUE (element) ||
              json_node_get_value_type (element) != G_TYPE_STRING)
            continue;

          path = artifact_path (request, json_node_get_string (element));

          if (path == NULL)
            continue;

          if (classpath->len > 0)
            g_string_append_c (classpath, ':');

          g_string_append (classpath, path);
        }
    }

  argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (request->java));
  g_ptr_array_add (argv, g_strdup ("-cp"));
  g_ptr_array_add (argv, g_strdup (classpath->str));
  g_ptr_array_add (argv, g_strdup (request->main_class));

  node = json_object_get_member (processor, "args");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          g_autofree char *argument = NULL;

          if (!JSON_NODE_HOLDS_VALUE (element) ||
              json_node_get_value_type (element) != G_TYPE_STRING)
            continue;

          argument = substitute_argument (
            request, json_node_get_string (element), &error);

          if (argument == NULL)
            {
              install_failed (request, g_steal_pointer (&error));
              return;
            }

          g_ptr_array_add (argv, g_steal_pointer (&argument));
        }
    }

  g_ptr_array_add (argv, NULL);

  {
    g_autoptr (GSubprocess) process = NULL;

    process = g_subprocess_newv (
      (const char *const *) argv->pdata,
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
      &error);

    if (process == NULL)
      {
        install_failed (request, g_steal_pointer (&error));
        return;
      }

    g_subprocess_communicate_utf8_async (
      process, NULL, g_task_get_cancellable (request->task),
      processor_finished, request);
  }
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
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_GENERATION,
                                   "the processors of %s %s did not leave a "
                                   "patched client jar behind, which is not a "
                                   "shape AML knows how to install",
                                   aml_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  dists = jar_manifest_attribute (request->patched_jar, "Minecraft-Dists");

  if (dists == NULL)
    dists = jar_manifest_attribute (request->patched_jar, "Minecraft-Dist");

  if (dists == NULL)
    {
      if (!write_jar_manifest_attribute (request->patched_jar,
                                         "Minecraft-Dists", "client",
                                         &error))
        {
          install_failed (request, g_steal_pointer (&error));
          return;
        }
    }

  install_finished (request, NULL);
}

static void
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

void
aml_loaders_install_async (AmlLoader              loader,
                           const char            *game_version,
                           const char            *loader_version,
                           AmlVersionDescription *game_document,
                           const char            *java_executable,
                           const char            *data_dir,
                           GCancellable          *cancellable,
                           AmlLoadersProgressFunc  progress,
                           gpointer                progress_data,
                           GAsyncReadyCallback     callback,
                           gpointer                user_data)
{
  InstallRequest *request = NULL;
  g_autofree char *versions_dir = NULL;
  g_autofree char *jar_name = NULL;

  g_return_if_fail (game_version != NULL);
  g_return_if_fail (loader_version != NULL);
  g_return_if_fail (AML_IS_VERSION_DESCRIPTION (game_document));
  g_return_if_fail (java_executable != NULL);

  request = g_new0 (InstallRequest, 1);
  request->task = g_task_new (NULL, cancellable, callback, user_data);
  request->scope = aml_task_scope_from_task (request->task);
  request->loader = loader;
  request->game_version = g_strdup (game_version);
  request->loader_version = g_strdup (loader_version);
  request->java = g_strdup (java_executable);
  request->pending = g_ptr_array_new_with_free_func (install_file_free);
  request->progress = progress;
  request->progress_data = progress_data;

  g_task_set_check_cancellable (request->task, FALSE);

  if (!aml_loader_publishes_an_installer (loader))
    {
      install_failed (request,
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_UNSUPPORTED,
                                   "%s publishes no installer to run",
                                   aml_loader_display_name (loader)));
      return;
    }

  if (data_dir != NULL)
    request->data_dir = g_strdup (data_dir);
  else
    request->data_dir = g_build_filename (g_get_user_data_dir (), "AML", NULL);

  /* Every loader version is installed in a directory of its own, with the
     artifacts of its profile in a maven beside it, so that the games and
     the installers share nothing and an unused version can be thrown away
     whole. */
  request->root = g_build_filename (request->data_dir, AML_LOADERS_SUBDIR,
                                    aml_loader_to_string (loader),
                                    loader_version, NULL);

  /* The artifacts of a profile go into the library directory of the games,
     which is where the document of the loader names them: the patched
     client jar among them, by its own coordinates. */
  request->libraries_dir = g_build_filename (request->data_dir, "libraries",
                                             NULL);

  jar_name = g_strdup_printf ("%s-%s-installer.jar",
                              aml_loader_to_string (loader), loader_version);
  request->installer = g_build_filename (request->root, jar_name, NULL);

  /* The client jar of the game version is what the processors patch, and
     the install fetches that one file of the game itself: the resolve
     module is never asked for the game of a loader, only for the loader
     document over it. */
  if (game_document->client == NULL || game_document->client->url == NULL)
    {
      install_failed (request,
                      g_error_new (AML_LOADERS_ERROR,
                                   AML_LOADERS_ERROR_INSTALLER,
                                   "Minecraft %s has no client jar to patch",
                                   game_version));
      return;
    }

  versions_dir = g_build_filename (request->data_dir, "versions",
                                   game_version, NULL);
  jar_name = g_strdup_printf ("%s.jar", game_version);
  request->client_jar = g_build_filename (versions_dir, jar_name, NULL);

  request->stage = STAGE_CLIENT_JAR;

  if (g_file_test (request->client_jar, G_FILE_TEST_IS_REGULAR))
    {
      stage_installer (request);
      return;
    }

  queue_file (request, FILE_FETCH, game_document->client->url, NULL,
              request->client_jar, game_document->client->sha1, FALSE);
  drain_pending (request);
}

AmlLoaderInstall *
aml_loaders_install_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
