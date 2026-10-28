/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-files-private.h"

#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static char *
make_jar (const char *member, const char *contents)
{
  char *path = g_strdup ("/tmp/nuvashalauncher-metadata-XXXXXX.jar");
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

static char *
make_jar_with_two_members (const char *member_one, const char *contents_one,
                           const char *member_two, const char *contents_two)
{
  char *path = g_strdup ("/tmp/nuvashalauncher-metadata-two-XXXXXX.jar");
  int fd = g_mkstemp (path);
  struct archive *archive = archive_write_new ();
  const char *members[] = { member_one, member_two };
  const char *contents[] = { contents_one, contents_two };

  g_assert_cmpint (fd, >=, 0);
  close (fd);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==, ARCHIVE_OK);
  for (guint i = 0; i < G_N_ELEMENTS (members); i++)
    {
      struct archive_entry *entry = archive_entry_new ();
      archive_entry_set_pathname (entry, members[i]);
      archive_entry_set_filetype (entry, AE_IFREG);
      archive_entry_set_perm (entry, 0644);
      archive_entry_set_size (entry, strlen (contents[i]));
      g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
      g_assert_cmpint (archive_write_data (archive, contents[i], strlen (contents[i])),
                       ==, (la_ssize_t) strlen (contents[i]));
      archive_entry_free (entry);
    }
  g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
  archive_write_free (archive);
  return path;
}

static char *
make_large_jar (void)
{
  g_autofree char *contents = g_malloc0 (1024 * 1024 + 1);
  char *path = g_strdup ("/tmp/nuvashalauncher-metadata-large-XXXXXX.jar");
  int fd = g_mkstemp (path);
  struct archive *archive = archive_write_new ();
  struct archive_entry *entry = archive_entry_new ();

  g_assert_cmpint (fd, >=, 0);
  close (fd);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==, ARCHIVE_OK);
  archive_entry_set_pathname (entry, "fabric.mod.json");
  archive_entry_set_filetype (entry, AE_IFREG);
  archive_entry_set_perm (entry, 0644);
  archive_entry_set_size (entry, 1024 * 1024 + 1);
  g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_data (archive, contents, 1024 * 1024 + 1),
                   ==, 1024 * 1024 + 1);
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
  g_autofree char *large = make_large_jar ();
  g_autofree char *quilt = make_jar (
    "quilt.mod.json",
    "{\"quilt_loader\":{\"id\":\"quilt-example\",\"version\":\"2.0\","
    "\"metadata\":{\"name\":\"Quilt Example\",\"description\":\"quilt desc\","
    "\"icon\":{\"16\":\"small.png\",\"64\":\"large.png\"}},"
    "\"depends\":{\"minecraft\":\"1.20\",\"qsl\":\"*\"}}}");
  g_autofree char *forge = make_jar (
    "META-INF/mods.toml",
    "modLoader=\"javafml\"\n[[mods]]\nmodId=\"forge_example\"\n"
    "version=\"3.0\"\ndisplayName=\"Forge Example\"\n"
    "description=\"forge desc\"\nlogoFile=\"logo.png\"\n");
  g_autofree char *mcmod = make_jar (
    "mcmod.info",
    "[{\"modid\":\"legacy\",\"name\":\"Legacy Example\","
    "\"description\":\"legacy desc\",\"version\":\"0.9\","
    "\"logoFile\":\"legacy.png\"}]");
  g_autofree char *pack = make_jar_with_two_members (
    "pack.mcmeta", "{\"pack\":{\"description\":\"Resource description\"}}",
    "ignored.txt", "not metadata");
  g_autoptr (nuvashalauncherContentFile) file = g_new0 (nuvashalauncherContentFile, 1);
  g_autoptr (nuvashalauncherContentFile) broken = g_new0 (nuvashalauncherContentFile, 1);
  g_autoptr (nuvashalauncherContentFile) oversized =
    g_new0 (nuvashalauncherContentFile, 1);
  g_autoptr (nuvashalauncherContentFile) quilt_file =
    g_new0 (nuvashalauncherContentFile, 1);
  g_autoptr (nuvashalauncherContentFile) forge_file =
    g_new0 (nuvashalauncherContentFile, 1);
  g_autoptr (nuvashalauncherContentFile) mcmod_file =
    g_new0 (nuvashalauncherContentFile, 1);
  g_autoptr (nuvashalauncherContentFile) pack_file =
    g_new0 (nuvashalauncherContentFile, 1);

  file->path = g_strdup (valid);
  nuvashalauncher_content_apply_local_metadata (file, nuvashalauncher_CONTENT_MOD);
  g_assert_true (file->local_metadata);
  g_assert_cmpstr (file->project_name, ==, "Example");
  g_assert_cmpstr (file->local_id, ==, "example");
  g_assert_cmpstr (file->version_name, ==, "1.2");
  g_assert_cmpuint (file->required_dependencies->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (file->required_dependencies, 0), ==,
                   "other");

  broken->path = g_strdup (truncated);
  nuvashalauncher_content_apply_local_metadata (broken, nuvashalauncher_CONTENT_MOD);
  g_assert_false (broken->local_metadata);
  oversized->path = g_strdup (large);
  nuvashalauncher_content_apply_local_metadata (oversized,
                                                nuvashalauncher_CONTENT_MOD);
  g_assert_false (oversized->local_metadata);
  g_assert_cmpint (g_unlink (valid), ==, 0);
  g_assert_cmpint (g_unlink (truncated), ==, 0);
  g_assert_cmpint (g_unlink (large), ==, 0);
  quilt_file->path = g_strdup (quilt);
  nuvashalauncher_content_apply_local_metadata (quilt_file, nuvashalauncher_CONTENT_MOD);
  g_assert_true (quilt_file->local_metadata);
  g_assert_cmpstr (quilt_file->project_name, ==, "Quilt Example");
  g_assert_cmpstr (quilt_file->local_id, ==, "quilt-example");
  g_assert_cmpstr (quilt_file->description, ==, "quilt desc");
  g_assert_cmpstr (g_ptr_array_index (quilt_file->required_dependencies, 0), ==, "qsl");
  g_assert_cmpstr (quilt_file->local_icon_member, ==, "large.png");

  forge_file->path = g_strdup (forge);
  nuvashalauncher_content_apply_local_metadata (forge_file, nuvashalauncher_CONTENT_MOD);
  g_assert_true (forge_file->local_metadata);
  g_assert_cmpstr (forge_file->project_name, ==, "Forge Example");
  g_assert_cmpstr (forge_file->local_id, ==, "forge_example");
  g_assert_cmpstr (forge_file->version_name, ==, "3.0");
  g_assert_cmpstr (forge_file->local_icon_member, ==, "logo.png");

  mcmod_file->path = g_strdup (mcmod);
  nuvashalauncher_content_apply_local_metadata (mcmod_file, nuvashalauncher_CONTENT_MOD);
  g_assert_true (mcmod_file->local_metadata);
  g_assert_cmpstr (mcmod_file->project_name, ==, "Legacy Example");
  g_assert_cmpstr (mcmod_file->local_id, ==, "legacy");
  g_assert_cmpstr (mcmod_file->version_name, ==, "0.9");

  pack_file->path = g_strdup (pack);
  nuvashalauncher_content_apply_local_metadata (
    pack_file, nuvashalauncher_CONTENT_RESOURCE_PACK);
  g_assert_true (pack_file->local_metadata);
  g_assert_cmpstr (pack_file->description, ==, "Resource description");

  g_assert_cmpint (g_unlink (quilt), ==, 0);
  g_assert_cmpint (g_unlink (forge), ==, 0);
  g_assert_cmpint (g_unlink (mcmod), ==, 0);
  g_assert_cmpint (g_unlink (pack), ==, 0);
  g_print ("ok - local metadata parsers handle Fabric, Quilt, Forge, legacy, and packs\n");
  return 0;
}
