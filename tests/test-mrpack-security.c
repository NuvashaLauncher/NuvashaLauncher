/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for the security of leppoisa-modpack: the paths a pack may name,
 * the hosts it may ask for, and the archives it may be.
 *
 * The .mrpack fixtures are written by the test itself out of nothing
 * but bytes, so no pack is ever checked in and nothing here needs the
 * network. Every fixture is a small program of its own: a traversal
 * here, a symlink there, a bomb of entries or of bytes. */

#include "leppoisa-modpack.h"

#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

static char *
make_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("leppoisa-mrpack-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (dir);
  return dir;
}

/* Synchronous recursive delete, test cleanup only. Symlinks are
 * removed, never followed. */
static void
remove_recursive (const char *path)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInfo) info =
    g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);

  if (info == NULL)
    return;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr (GFileEnumerator) enumerator =
        g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   NULL, &error);

      if (enumerator != NULL)
        {
          GFileInfo *child = NULL;

          while ((child =
                    g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
            {
              g_autoptr (GFile) child_file =
                g_file_get_child (file, g_file_info_get_name (child));
              g_autofree char *child_path = g_file_get_path (child_file);
              g_object_unref (child);
              remove_recursive (child_path);
            }
        }
    }

  g_file_delete (file, NULL, NULL);
}

/* An index of a pack with the members the format requires. */
static char *
pack_index (const char *name)
{
  return g_strdup_printf (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1.0.0\","
    "\"name\":\"%s\",\"files\":[],\"dependencies\":{\"minecraft\":\"1.20.1\"}}",
    name);
}

/* One entry of a .mrpack the test writes. */
typedef struct
{
  const char *name;
  const char *data;
  gsize size;
  guint type;       /* AE_IFREG, AE_IFDIR, AE_IFLNK */
  const char *link; /* symlink target when type is AE_IFLNK */
} ZipEntry;

/* A regular file entry out of a literal. */
#define ZIP_FILE(name, data) \
  { name, data, sizeof (data) - 1, AE_IFREG, NULL }

/* Writes a .mrpack out of the entries, in the order given. A symlink
 * entry carries its target and nothing else: the writer puts the
 * target in the entry itself, and writing data alongside it would
 * make the archive inconsistent. */
static void
zip_write (const char *path, const ZipEntry *entries, gsize count)
{
  struct archive *archive = archive_write_new ();

  g_assert_nonnull (archive);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==,
                   ARCHIVE_OK);

  for (gsize i = 0; i < count; i++)
    {
      struct archive_entry *entry = archive_entry_new ();

      g_assert_nonnull (entry);
      archive_entry_set_pathname (entry, entries[i].name);
      archive_entry_set_filetype (entry, entries[i].type);
      archive_entry_set_perm (entry,
                              entries[i].type == AE_IFDIR ? 0755 : 0644);

      if (entries[i].type == AE_IFLNK)
        {
          archive_entry_set_symlink (entry, entries[i].link);
          archive_entry_set_size (entry,
                                  (la_int64_t) strlen (entries[i].link));
        }
      else if (entries[i].type == AE_IFREG)
        {
          archive_entry_set_size (entry, (la_int64_t) entries[i].size);
        }

      g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);

      if (entries[i].type == AE_IFREG && entries[i].size > 0)
        g_assert_cmpint (
          (int) archive_write_data (archive, entries[i].data,
                                     entries[i].size),
          ==, (int) entries[i].size);

      archive_entry_free (entry);
    }

  g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
  archive_write_free (archive);
}

/* Writes a .mrpack whose entries are the index and then @count small
 * files, for the bomb of entries. */
static void
zip_write_entry_bomb (const char *path, const char *index_json, guint count)
{
  struct archive *archive = archive_write_new ();
  g_autofree char *block = g_malloc0 (1024);

  g_assert_nonnull (archive);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==,
                   ARCHIVE_OK);

  {
    struct archive_entry *entry = archive_entry_new ();

    archive_entry_set_pathname (entry, "modrinth.index.json");
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0644);
    archive_entry_set_size (entry, (la_int64_t) strlen (index_json));
    g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
    g_assert_cmpint ((int) archive_write_data (archive, index_json,
                                               strlen (index_json)),
                     ==, (int) strlen (index_json));
    archive_entry_free (entry);
  }

  for (guint i = 0; i < count; i++)
    {
      struct archive_entry *entry = archive_entry_new ();
      g_autofree char *name = g_strdup_printf ("f%05u", i);

      archive_entry_set_pathname (entry, name);
      archive_entry_set_filetype (entry, AE_IFREG);
      archive_entry_set_perm (entry, 0644);
      archive_entry_set_size (entry, 1024);
      g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
      g_assert_cmpint ((int) archive_write_data (archive, block, 1024), ==,
                       1024);
      archive_entry_free (entry);
    }

  g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
  archive_write_free (archive);
}

/* Writes a .mrpack whose index announces more content than one pack
 * carries. The bytes are real: the reader counts what the header
 * announces, and a header that lies about its size is a broken
 * archive, not a bomb. */
static void
zip_write_size_bomb (const char *path, const char *index_json,
                     guint mebibytes)
{
  struct archive *archive = archive_write_new ();
  g_autofree char *block = g_malloc0 (1024 * 1024);
  guint64 left = (guint64) mebibytes * 1024 * 1024;

  g_assert_nonnull (archive);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==,
                   ARCHIVE_OK);

  {
    struct archive_entry *entry = archive_entry_new ();

    archive_entry_set_pathname (entry, "modrinth.index.json");
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0644);
    archive_entry_set_size (entry, (la_int64_t) strlen (index_json));
    g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
    g_assert_cmpint ((int) archive_write_data (archive, index_json,
                                               strlen (index_json)),
                     ==, (int) strlen (index_json));
    archive_entry_free (entry);
  }

  {
    struct archive_entry *entry = archive_entry_new ();

    archive_entry_set_pathname (entry, "big.bin");
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0644);
    archive_entry_set_size (entry, (la_int64_t) left);
    g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
    archive_entry_free (entry);

    while (left > 0)
      {
        gsize take = MIN (left, 1024 * 1024);

        g_assert_cmpint ((int) archive_write_data (archive, block, take), ==,
                         (int) take);
        left -= take;
      }
  }

  g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
  archive_write_free (archive);
}

/* Writes a .mrpack whose index is larger than one manifest can be. */
static void
zip_write_index_bomb (const char *path, guint mebibytes)
{
  struct archive *archive = archive_write_new ();
  g_autofree char *block = g_malloc0 (1024 * 1024);
  guint64 left = (guint64) mebibytes * 1024 * 1024;

  g_assert_nonnull (archive);
  g_assert_cmpint (archive_write_set_format_zip (archive), ==, ARCHIVE_OK);
  g_assert_cmpint (archive_write_open_filename (archive, path), ==,
                   ARCHIVE_OK);

  {
    struct archive_entry *entry = archive_entry_new ();

    archive_entry_set_pathname (entry, "modrinth.index.json");
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0644);
    archive_entry_set_size (entry, (la_int64_t) left);
    g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
    archive_entry_free (entry);

    while (left > 0)
      {
        gsize take = MIN (left, 1024 * 1024);

        g_assert_cmpint ((int) archive_write_data (archive, block, take), ==,
                         (int) take);
        left -= take;
      }
  }

  g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
  archive_write_free (archive);
}

/* Writes a .mrpack whose index carries a raw NUL byte. */
static void
zip_write_nul_index (const char *path, const char *index_json)
{
  gsize length = strlen (index_json);
  g_autofree char *bytes = g_malloc (length);
  ZipEntry entry;

  memcpy (bytes, index_json, length);
  bytes[length / 2] = '\0';

  entry.name = "modrinth.index.json";
  entry.data = bytes;
  entry.size = length;
  entry.type = AE_IFREG;
  entry.link = NULL;

  zip_write (path, &entry, 1);
}

/* Writes a file that is not a zip at all. */
static void
write_not_a_zip (const char *path)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_set_contents (path, "this is not a zip", -1, &error));
  g_assert_no_error (error);
}

/* The archive is refused, and for the reason the test names. */
static void
expect_zip_refused (const char *what, const char *zip_path, guint code)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (LeppoisaModpack) pack = NULL;

  pack = leppoisa_modpack_new_from_zip (zip_path, &error);
  g_assert_null (pack);
  g_assert_error (error, leppoisa_MODPACK_ERROR, code);
  g_print ("ok - %s\n", what);
}

/* ---------------------------------------------------------- *
 * The paths a pack may name
 * ---------------------------------------------------------- */

static void
test_path_is_safe (void)
{
  /* What a pack may name. */
  g_assert_true (leppoisa_modpack_path_is_safe ("mods/sodium.jar"));
  g_assert_true (leppoisa_modpack_path_is_safe ("config/x.toml"));
  g_assert_true (leppoisa_modpack_path_is_safe ("options.txt"));
  g_assert_true (leppoisa_modpack_path_is_safe ("a/b/c/d.txt"));

  /* What a pack may not name. */
  g_assert_false (leppoisa_modpack_path_is_safe (NULL));
  g_assert_false (leppoisa_modpack_path_is_safe (""));
  g_assert_false (leppoisa_modpack_path_is_safe ("../x"));
  g_assert_false (leppoisa_modpack_path_is_safe ("a/../b"));
  g_assert_false (leppoisa_modpack_path_is_safe (".."));
  g_assert_false (leppoisa_modpack_path_is_safe ("."));
  g_assert_false (leppoisa_modpack_path_is_safe ("a/./b"));
  g_assert_false (leppoisa_modpack_path_is_safe ("/etc/passwd"));
  g_assert_false (leppoisa_modpack_path_is_safe ("a\\b"));
  g_assert_false (leppoisa_modpack_path_is_safe ("C:/x"));
  g_assert_false (leppoisa_modpack_path_is_safe ("C:"));
  g_assert_false (leppoisa_modpack_path_is_safe ("a\nb"));
  g_assert_false (leppoisa_modpack_path_is_safe ("a\tb"));
  g_assert_false (leppoisa_modpack_path_is_safe ("a\x01" "b"));

  /* DEL, the one control byte above the printable range. */
  g_assert_false (leppoisa_modpack_path_is_safe ("a\x7f" "b"));

  /* A trailing / names an empty part. A directory entry of the
   * archive has its / taken off before this check, so what reaches
   * here is a name without one. */
  g_assert_false (leppoisa_modpack_path_is_safe ("mods/"));

  g_print ("ok - a path the instance can hold is told from one it cannot\n");
}

/* ---------------------------------------------------------- *
 * The hosts a pack may ask for
 * ---------------------------------------------------------- */

static void
test_download_url_allowed (void)
{
  /* The four hosts the format has for the files of a pack. */
  g_assert_true (
    leppoisa_modpack_download_url_allowed ("https://cdn.modrinth.com/data/x"));
  g_assert_true (
    leppoisa_modpack_download_url_allowed ("https://github.com/u/r/raw/f"));
  g_assert_true (leppoisa_modpack_download_url_allowed (
    "https://raw.githubusercontent.com/u/r/f"));
  g_assert_true (
    leppoisa_modpack_download_url_allowed ("https://gitlab.com/u/r"));

  /* Anything else: another scheme, another host, a host with a port
   * of its own, credentials in front of it, or a path that merely
   * walks through its name. */
  g_assert_false (
    leppoisa_modpack_download_url_allowed ("http://cdn.modrinth.com/x"));
  g_assert_false (leppoisa_modpack_download_url_allowed ("https://evil.com/x"));
  g_assert_false (
    leppoisa_modpack_download_url_allowed ("https://cdn.modrinth.com.evil.com/x"));
  g_assert_false (
    leppoisa_modpack_download_url_allowed ("https://cdn.modrinth.com@evil.com/x"));
  g_assert_false (
    leppoisa_modpack_download_url_allowed ("https://user@github.com/x"));
  g_assert_false (
    leppoisa_modpack_download_url_allowed ("https://cdn.modrinth.com:8443/x"));
  g_assert_false (
    leppoisa_modpack_download_url_allowed ("https://evil.com/cdn.modrinth.com"));

  g_print ("ok - a download URL is asked of the hosts the format names\n");
}

/* ---------------------------------------------------------- *
 * The archives a pack may be
 * ---------------------------------------------------------- */

static void
test_archive_paths_are_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *index_json = pack_index ("Traversal");
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  /* A path that steps out of the instance. */
  {
    ZipEntry entries[] = {
      { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
        NULL },
      ZIP_FILE ("../escape.txt", "outside"),
    };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
    expect_zip_refused ("an entry that steps out of the instance", zip_path,
                        leppoisa_MODPACK_ERROR_PATH);
  }

  /* An absolute path. */
  {
    ZipEntry entries[] = {
      { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
        NULL },
      ZIP_FILE ("/tmp/escape.txt", "outside"),
    };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
    expect_zip_refused ("an entry with an absolute path", zip_path,
                        leppoisa_MODPACK_ERROR_PATH);
  }

  /* A path with the separator of another system. */
  {
    ZipEntry entries[] = {
      { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
        NULL },
      ZIP_FILE ("mods\\..\\escape.txt", "outside"),
    };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
    expect_zip_refused ("an entry with a backslash in it", zip_path,
                        leppoisa_MODPACK_ERROR_PATH);
  }

  /* A drive name. */
  {
    ZipEntry entries[] = {
      { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
        NULL },
      ZIP_FILE ("C:/escape.txt", "outside"),
    };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
    expect_zip_refused ("an entry with a drive name in it", zip_path,
                        leppoisa_MODPACK_ERROR_PATH);
  }

  remove_recursive (dir);
  g_print ("ok - an archive that names a path outside is refused\n");
}

static void
test_archive_links_are_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *index_json = pack_index ("Links");
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  /* A symlink entry: it would point out of the instance. */
  {
    ZipEntry entries[] = {
      { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
        NULL },
      { "overrides/link", NULL, 0, AE_IFLNK, "../../outside" },
    };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
    expect_zip_refused ("an archive that holds a symlink", zip_path,
                        leppoisa_MODPACK_ERROR_PATH);
  }

  /* A file through a symlink: the archive is refused at the symlink,
   * so no path is ever written through it. (A hardlink cannot be
   * written into a zip at all: the format has no field for one, and
   * the writer stores it as an empty file. The check for it is
   * defense in depth.) */
  {
    ZipEntry entries[] = {
      { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
        NULL },
      { "overrides/link", NULL, 0, AE_IFLNK, "../../outside" },
      ZIP_FILE ("overrides/link/payload.txt", "through the link"),
    };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
    expect_zip_refused ("a file written through a symlink", zip_path,
                        leppoisa_MODPACK_ERROR_PATH);
  }

  remove_recursive (dir);
  g_print ("ok - an archive that holds a link is refused\n");
}

static void
test_archive_nul_index_is_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *index_json = pack_index ("NUL");
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  zip_write_nul_index (zip_path, index_json);
  expect_zip_refused ("an index that carries a NUL byte", zip_path,
                      leppoisa_MODPACK_ERROR_PATH);

  remove_recursive (dir);
  g_print ("ok - an index with a raw NUL byte is refused\n");
}

static void
test_archive_without_index_is_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  {
    ZipEntry entries[] = { ZIP_FILE ("readme.txt", "no index in here") };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
    expect_zip_refused ("an archive with no modrinth.index.json", zip_path,
                        leppoisa_MODPACK_ERROR_FORMAT);
  }

  remove_recursive (dir);
  g_print ("ok - an archive with no index is refused\n");
}

static void
test_not_a_zip_is_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  write_not_a_zip (zip_path);
  expect_zip_refused ("a file that is not a zip", zip_path,
                      leppoisa_MODPACK_ERROR_ARCHIVE);

  remove_recursive (dir);
  g_print ("ok - a file that is not a zip is refused\n");
}

static void
test_archive_entry_bomb_is_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *index_json = pack_index ("Bomb");
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  /* The index plus ten thousand small files: one more entry than one
   * pack carries. */
  zip_write_entry_bomb (zip_path, index_json, leppoisa_MODPACK_MAX_ENTRIES);
  expect_zip_refused ("an archive with more entries than one pack", zip_path,
                      leppoisa_MODPACK_ERROR_BOMB);

  remove_recursive (dir);
  g_print ("ok - an archive of too many entries is refused\n");
}

static void
test_archive_size_bomb_is_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *index_json = pack_index ("Bomb");
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  /* The real cap is four gigabytes: too much to build here, so the cap
     is lowered to what the archive below crosses. What is checked is
     that an archive past whatever the cap is is refused, not the size of
     the cap itself. */
  leppoisa_modpack_set_content_limit ((guint64) 128 * 1024 * 1024);

  /* One entry that announces more content than one pack carries. */
  zip_write_size_bomb (zip_path, index_json, 129);
  expect_zip_refused ("an archive that announces too much content", zip_path,
                      leppoisa_MODPACK_ERROR_BOMB);

  leppoisa_modpack_set_content_limit (leppoisa_MODPACK_MAX_CONTENT_SIZE);
  remove_recursive (dir);
  g_print ("ok - an archive of too much content is refused\n");
}

static void
test_archive_index_bomb_is_refused (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);

  /* An index that is larger than one manifest can be. */
  zip_write_index_bomb (zip_path, 9);
  expect_zip_refused ("an index that decodes past one manifest", zip_path,
                      leppoisa_MODPACK_ERROR_BOMB);

  remove_recursive (dir);
  g_print ("ok - an index of too many bytes is refused\n");
}

static void
test_safe_archive_opens (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *index_json = pack_index ("Safe");
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (LeppoisaModpack) pack = NULL;

  {
    ZipEntry entries[] = {
      { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
        NULL },
      ZIP_FILE ("overrides/config/options.txt", "options"),
      ZIP_FILE ("client-overrides/config/client.txt", "client"),
    };

    zip_write (zip_path, entries, G_N_ELEMENTS (entries));
  }

  pack = leppoisa_modpack_new_from_zip (zip_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_cmpstr (pack->archive_path, ==, zip_path);

  remove_recursive (dir);
  g_print ("ok - an archive that is a pack opens\n");
}

/* ---------------------------------------------------------- */

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/mrpack/path", test_path_is_safe);
  g_test_add_func ("/mrpack/url", test_download_url_allowed);
  g_test_add_func ("/mrpack/archive-paths", test_archive_paths_are_refused);
  g_test_add_func ("/mrpack/archive-links", test_archive_links_are_refused);
  g_test_add_func ("/mrpack/archive-nul-index",
                   test_archive_nul_index_is_refused);
  g_test_add_func ("/mrpack/archive-no-index",
                   test_archive_without_index_is_refused);
  g_test_add_func ("/mrpack/not-a-zip", test_not_a_zip_is_refused);
  g_test_add_func ("/mrpack/archive-entry-bomb",
                   test_archive_entry_bomb_is_refused);
  g_test_add_func ("/mrpack/archive-size-bomb",
                   test_archive_size_bomb_is_refused);
  g_test_add_func ("/mrpack/archive-index-bomb",
                   test_archive_index_bomb_is_refused);
  g_test_add_func ("/mrpack/safe-archive", test_safe_archive_opens);

  return g_test_run ();
}
