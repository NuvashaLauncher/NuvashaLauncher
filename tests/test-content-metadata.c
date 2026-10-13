/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-files-private.h"

#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>

static char *
make_jar (const char *member, const char *contents)
{
  char *path = g_strdup ("/tmp/aml-metadata-XXXXXX.jar");
  int fd = g_mkstemp (path);
  struct archive *archive = archive_write_new ();
  struct archive_entry *entry = archive_entry_new ();

  g_assert_cmpint (fd, >=, 0);
  close (fd);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==, ARCHIVE_OK);
  archive_entry_set_pathname (entry, member);
  archive_entry_set_filetype (entry, AE_IFREG);
  archive_entry_set_perm (entry, 0644);
  archive_entry_set_size (entry, strlen (contents));
  g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_data (archive, contents, strlen (contents)),
                  ==, (la_ssize_t) strlen (contents));
  archive_entry_free (entry);
  g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
  archive_write_free (archive);
  return path;
}

int
main (void)
{
  g_autofree char *valid = make_jar (
    "fabric.mod.json",
    "{\"id\":\"example\",\"name\":\"Example\","
    "\"version\":\"1.2\",\"depends\":{\"other\":\"1\"}}");
  g_autofree char *truncated = make_jar (
    "fabric.mod.json", "{\"id\":\"example\",\"depends\":{\"other");
  g_autoptr (AmlContentFile) file = g_new0 (AmlContentFile, 1);
  g_autoptr (AmlContentFile) broken = g_new0 (AmlContentFile, 1);

  file->path = g_strdup (valid);
  aml_content_apply_local_metadata (file, AML_CONTENT_MOD);
  g_assert_true (file->local_metadata);
  g_assert_cmpstr (file->project_name, ==, "Example");
  g_assert_cmpstr (file->local_id, ==, "example");
  g_assert_cmpstr (file->version_name, ==, "1.2");
  g_assert_cmpuint (file->required_dependencies->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (file->required_dependencies, 0), ==,
                   "other");

  broken->path = g_strdup (truncated);
  aml_content_apply_local_metadata (broken, AML_CONTENT_MOD);
  g_assert_false (broken->local_metadata);
  g_print ("ok - local metadata parsers handle valid and truncated JSON\n");
  return 0;
}
