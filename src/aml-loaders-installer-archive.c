/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Archive and jar utilities used by loader installation. */

#include "aml-loaders.h"
#include "aml-loaders-private.h"
#include "aml-fs.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INSTALLER_ENTRY_SIZE (128ULL * 1024ULL * 1024ULL)

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

gboolean
aml_loader_ensure_directory (const char *path, GError **error)
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
gboolean
aml_loader_write_install_file (const char *path, const void *data, gsize size,
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
GBytes *
aml_loader_read_zip_entry (const char *path, const char *entry, GError **error)
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
char *
aml_loader_jar_manifest_attribute (const char *path, const char *name)
{
  g_autoptr (GBytes) manifest = NULL;
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  const char *data = NULL;
  gsize size = 0;
  g_autofree char *prefix = NULL;

  manifest = aml_loader_read_zip_entry (path, "META-INF/MANIFEST.MF", NULL);

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
gboolean
aml_loader_write_jar_manifest_attribute (const char *path, const char *name,
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

          bytes = aml_loader_read_zip_entry (path, "META-INF/MANIFEST.MF", NULL);

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
char *
aml_loader_jar_main_class (const char *path, GError **error)
{
  g_autoptr (GBytes) manifest = NULL;
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  const char *data = NULL;
  gsize size = 0;

  manifest = aml_loader_read_zip_entry (path, "META-INF/MANIFEST.MF", error);

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

