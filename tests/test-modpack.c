/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for nuvasha-modpack: what the index of a pack says, what its
 * dependencies resolve to, what the two hashes of a file prove, and
 * what an install leaves behind when it cannot go through.
 *
 * Everything here runs on files in a temporary directory: no network,
 * no display, nothing of the real data directory. The .mrpack
 * fixtures are written by the test itself, so no pack is ever checked
 * in. The install that succeeds never downloads anything: a pack with
 * no files and no loader installs from its overrides alone, which is
 * the whole pipeline minus the network. */

#include "nuvasha-download-modpack-dialog.h"
#include "nuvasha-modpack.h"

#include <gtk/gtk.h>

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
  char *dir = g_dir_make_tmp ("nuvasha-modpack-XXXXXX", &error);

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

static char *
read_file (const char *path)
{
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, &error))
    g_error ("cannot read %s: %s", path, error->message);

  return contents;
}

/* An index of a pack with the members the format requires. @files is
 * what goes inside the files array, brackets and commas included: an
 * empty pack passes "" and one file passes "{...}". */
static char *
pack_index (const char *name, const char *files, const char *deps)
{
  return g_strdup_printf (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1.0.0\","
    "\"name\":\"%s\",\"summary\":\"a pack the tests made\","
    "\"files\":[%s],\"dependencies\":{%s}}",
    name, files, deps);
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

/* Whether a directory holds a temporary directory of an install. */
static gboolean
has_temp_dir (const char *directory)
{
  GDir *dir = g_dir_open (directory, 0, NULL);
  const char *name = NULL;
  gboolean found = FALSE;

  if (dir == NULL)
    return FALSE;

  while ((name = g_dir_read_name (dir)) != NULL)
    if (g_str_has_prefix (name, ".nuvasha-modpack-"))
      {
        found = TRUE;
        break;
      }

  g_dir_close (dir);
  return found;
}

/* ---------------------------------------------------------- *
 * Waiting for what reports back
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  GError *error;
  gboolean value;
  NuvashaInstance *instance;
  gboolean timed_out;
  guint watchdog;
  NuvashaTaskScope *operation;
} AsyncWait;

static gboolean
async_timeout (gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->timed_out = TRUE;
  wait->watchdog = 0;
  g_main_loop_quit (wait->loop);
  return G_SOURCE_REMOVE;
}

static void
async_wait_run (AsyncWait *wait)
{
  wait->loop = g_main_loop_new (NULL, FALSE);
  wait->watchdog = g_timeout_add_seconds (30, async_timeout, wait);
  g_main_loop_run (wait->loop);

  if (wait->watchdog != 0)
    g_source_remove (wait->watchdog);

  g_main_loop_unref (wait->loop);
  wait->loop = NULL;

  g_assert_false (wait->timed_out);
}

static void
verify_file_done (GObject *source G_GNUC_UNUSED,
                  GAsyncResult *result,
                  gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->value = nuvasha_modpack_verify_file_finish (result, &wait->error);
  nuvasha_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

/* Both hashes of the manifest against the bytes on disk, in place. */
static gboolean
verify_now (const NuvashaModpackFile *file, const char *path, GError **error)
{
  AsyncWait wait = { 0 };

  wait.operation = nuvasha_modpack_verify_file_async (file, path, verify_file_done,
                                                 &wait);
  async_wait_run (&wait);

  *error = wait.error;
  return wait.value;
}

static void
install_done (GObject *source G_GNUC_UNUSED,
              GAsyncResult *result,
              gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->instance = nuvasha_modpack_install_finish (result, &wait->error);
  if (wait->operation != NULL)
    {
      nuvasha_task_scope_unref (wait->operation);
      wait->operation = NULL;
    }
  g_main_loop_quit (wait->loop);
}

static void
export_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  AsyncWait *wait = user_data;

  wait->value = nuvasha_modpack_export_finish (result, &wait->error);
  nuvasha_task_scope_unref (wait->operation);
  wait->operation = NULL;
  g_main_loop_quit (wait->loop);
}

/* The whole install, in place. */
static NuvashaInstance *
install_now (NuvashaModpack *pack, NuvashaInstanceManager *manager, const char *name,
             const char *directory, GError **error)
{
  AsyncWait wait = { 0 };

   wait.operation = nuvasha_modpack_install_async (
     pack, manager, name, directory, NULL, NULL, install_done, &wait);
  async_wait_run (&wait);

  *error = wait.error;
  return wait.instance;
}

static gboolean
export_now (NuvashaInstance *instance, const char *path, GError **error)
{
  AsyncWait wait = { 0 };

  wait.operation = nuvasha_modpack_export_async (instance, path, NULL, NULL, NULL,
                                             FALSE, NULL, NULL, export_done,
                                             &wait);
  async_wait_run (&wait);

  *error = wait.error;
  return wait.value;
}

static guint
manager_count (NuvashaInstanceManager *manager)
{
  return g_list_model_get_n_items (
    G_LIST_MODEL (nuvasha_instance_manager_get_instances (manager)));
}

/* ---------------------------------------------------------- *
 * What the index of a pack says
 * ---------------------------------------------------------- */

/* Four files that between them carry every state of env: required,
 * optional, unsupported, and no env object at all. */
static const char *FILES_JSON =
  "{\"path\":\"mods/sodium.jar\","
  "\"hashes\":{\"sha1\":\"0123456789abcdef0123456789abcdef01234567\","
  "\"sha512\":\"0123456789abcdef0123456789abcdef\"},"
  "\"fileSize\":12345,"
  "\"downloads\":[\"https://cdn.modrinth.com/data/aa/v1/sodium.jar\"],"
  "\"env\":{\"client\":\"required\",\"server\":\"unsupported\"}},"
  "{\"path\":\"mods/lithium.jar\","
  "\"hashes\":{\"sha1\":\"1123456789abcdef0123456789abcdef01234567\","
  "\"sha512\":\"1123456789abcdef0123456789abcdef\"},"
  "\"fileSize\":6789,"
  "\"downloads\":[\"https://cdn.modrinth.com/data/bb/v1/lithium.jar\"],"
  "\"env\":{\"client\":\"optional\",\"server\":\"required\"}},"
  "{\"path\":\"mods/caffeine.jar\","
  "\"hashes\":{\"sha1\":\"2123456789abcdef0123456789abcdef01234567\","
  "\"sha512\":\"2123456789abcdef0123456789abcdef\"},"
  "\"fileSize\":111,"
  "\"downloads\":[\"https://cdn.modrinth.com/data/cc/v1/caffeine.jar\"],"
  "\"env\":{\"client\":\"unsupported\",\"server\":\"required\"}},"
  "{\"path\":\"config/both.toml\","
  "\"hashes\":{\"sha1\":\"3123456789abcdef0123456789abcdef01234567\","
  "\"sha512\":\"3123456789abcdef0123456789abcdef\"},"
  "\"fileSize\":42,"
  "\"downloads\":[\"https://cdn.modrinth.com/data/dd/v1/both.toml\"]}";

static void
test_index_is_read (void)
{
  g_autofree char *json =
    pack_index ("Example Pack", FILES_JSON,
                "\"minecraft\":\"1.20.1\",\"fabric-loader\":\"0.15.11\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;
  NuvashaModpackFile *first = NULL;
  NuvashaModpackFile *last = NULL;

  pack = nuvasha_modpack_new_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);

  g_assert_cmpuint (pack->format_version, ==, 1);
  g_assert_cmpstr (pack->game, ==, "minecraft");
  g_assert_cmpstr (pack->version_id, ==, "1.0.0");
  g_assert_cmpstr (pack->name, ==, "Example Pack");
  g_assert_cmpstr (pack->summary, ==, "a pack the tests made");
  g_assert_null (pack->archive_path);
  g_assert_cmpuint (pack->files->len, ==, 4);
  g_assert_cmpuint (pack->dependencies->len, ==, 2);

  first = g_ptr_array_index (pack->files, 0);
  g_assert_cmpstr (first->path, ==, "mods/sodium.jar");
  g_assert_cmpstr (first->sha1, ==,
                   "0123456789abcdef0123456789abcdef01234567");
  g_assert_cmpstr (first->sha512, ==,
                   "0123456789abcdef0123456789abcdef");
  g_assert_cmpint ((gint) first->file_size, ==, 12345);
  g_assert_cmpint (first->env_client, ==, nuvasha_MODPACK_ENV_REQUIRED);
  g_assert_cmpint (first->env_server, ==, nuvasha_MODPACK_ENV_UNSUPPORTED);
  g_assert_cmpuint (first->downloads->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (first->downloads, 0), ==,
                   "https://cdn.modrinth.com/data/aa/v1/sodium.jar");

  /* The last file has no env object: what both sides can run. */
  last = g_ptr_array_index (pack->files, 3);
  g_assert_cmpstr (last->path, ==, "config/both.toml");
  g_assert_cmpint (last->env_client, ==, nuvasha_MODPACK_ENV_UNSPECIFIED);
  g_assert_cmpint (last->env_server, ==, nuvasha_MODPACK_ENV_UNSPECIFIED);
  g_assert_cmpint ((gint) last->file_size, ==, 42);

  g_print ("ok - the index of a pack is read whole\n");
}

static void
test_client_files_by_env (void)
{
  g_autofree char *json =
    pack_index ("Example Pack", FILES_JSON,
                "\"minecraft\":\"1.20.1\",\"fabric-loader\":\"0.15.11\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;
  g_autoptr (GPtrArray) required = NULL;
  g_autoptr (GPtrArray) optional = NULL;

  pack = nuvasha_modpack_new_from_json (json, -1, &error);
  g_assert_no_error (error);

  required = nuvasha_modpack_required_client_files (pack);
  optional = nuvasha_modpack_optional_client_files (pack);

  /* The client installs what it is required to have and what both
   * sides can run; what it is only offered it is offered, not saddled
   * with; what it must not see is never listed. */
  g_assert_cmpuint (required->len, ==, 2);
  g_assert_cmpstr (((NuvashaModpackFile *) g_ptr_array_index (required, 0))->path,
                   ==, "mods/sodium.jar");
  g_assert_cmpstr (((NuvashaModpackFile *) g_ptr_array_index (required, 1))->path,
                   ==, "config/both.toml");

  g_assert_cmpuint (optional->len, ==, 1);
  g_assert_cmpstr (((NuvashaModpackFile *) g_ptr_array_index (optional, 0))->path,
                   ==, "mods/lithium.jar");

  g_print ("ok - the env of a file says which list it belongs to\n");
}

/* ---------------------------------------------------------- *
 * What the dependencies resolve to
 * ---------------------------------------------------------- */

static void
assert_loader (const char *deps_json, NuvashaLoader loader,
               const char *minecraft, const char *loader_version)
{
  g_autofree char *json = pack_index ("Resolve Pack", "", deps_json);
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;

  pack = nuvasha_modpack_new_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvasha_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (pack->minecraft_version, ==, minecraft);
  g_assert_cmpint (pack->loader, ==, loader);
  g_assert_cmpstr (pack->loader_version, ==, loader_version);
}

static void
test_resolve_fabric (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"fabric-loader\":\"0.15.11\"",
                 nuvasha_LOADER_FABRIC, "1.20.1", "0.15.11");
  g_print ("ok - a pack that names fabric-loader resolves to fabric\n");
}

static void
test_resolve_quilt (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"quilt-loader\":\"0.21.0\"",
                 nuvasha_LOADER_QUILT, "1.20.1", "0.21.0");
  g_print ("ok - a pack that names quilt-loader resolves to quilt\n");
}

static void
test_resolve_forge (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"forge\":\"47.2.0\"",
                 nuvasha_LOADER_FORGE, "1.20.1", "47.2.0");
  g_print ("ok - a pack that names forge resolves to forge\n");
}

static void
test_resolve_neoforge (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"neoforge\":\"20.4.0\"",
                 nuvasha_LOADER_NEOFORGE, "1.20.1", "20.4.0");
  g_print ("ok - a pack that names neoforge resolves to neoforge\n");
}

static void
test_resolve_vanilla (void)
{
  /* A pack that names no loader runs on vanilla, and a name the
   * format does not know is ignored rather than refused. */
  assert_loader ("\"minecraft\":\"1.21\",\"some-future-loader\":\"9.9\"",
                 nuvasha_LOADER_VANILLA, "1.21", NULL);
  g_print ("ok - a pack that names no loader resolves to vanilla\n");
}

static void
test_resolve_loader_choice (void)
{
  /* Two loaders named: the one the format puts first wins. */
  assert_loader ("\"minecraft\":\"1.20.1\",\"forge\":\"47.2.0\","
                 "\"fabric-loader\":\"0.15.11\"",
                 nuvasha_LOADER_FABRIC, "1.20.1", "0.15.11");
  g_print ("ok - of two loaders named, the first of the format wins\n");
}

static void
test_resolve_without_minecraft (void)
{
  g_autofree char *json =
    pack_index ("No Minecraft", "", "\"fabric-loader\":\"0.15.11\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;

  pack = nuvasha_modpack_new_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_false (nuvasha_modpack_resolve (pack, &error));
  g_assert_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_LOADER);

  g_print ("ok - a pack that names no Minecraft version is refused\n");
}

/* ---------------------------------------------------------- *
 * What the format refuses
 * ---------------------------------------------------------- */

static void
expect_refused (const char *json, guint code, const char *what)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;

  pack = nuvasha_modpack_new_from_json (json, -1, &error);
  g_assert_null (pack);
  g_assert_error (error, nuvasha_MODPACK_ERROR, code);
  g_print ("ok - %s\n", what);
}

static void
test_format_refusals (void)
{
  /* A format version other than the one the format has today. */
  expect_refused (
    "{\"formatVersion\":2,\"game\":\"minecraft\",\"versionId\":\"2\","
    "\"name\":\"Too New\",\"files\":[],\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_FORMAT, "a format version nuvasha does not know");

  /* A pack for another game. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"dwarf-fortress\",\"versionId\":\"1\","
    "\"name\":\"Not Minecraft\",\"files\":[],\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_FORMAT, "a pack for another game");

  /* A file with one hash missing. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Half Hashed\",\"files\":["
    "{\"path\":\"mods/x.jar\",\"hashes\":{\"sha1\":\"aa\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/x.jar\"]}],"
    "\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_FORMAT, "a file with one hash missing");

  /* No files array at all. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"No Files\",\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_FORMAT, "an index without a files array");

  /* No name. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"files\":[],\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_FORMAT, "an index without a name");

  /* A files member that is not an array. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Odd Files\",\"files\":{},\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_FORMAT, "a files member that is not an array");

  /* An entry of files that is not an object. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Odd Entry\",\"files\":[\"mods/x.jar\"],\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_FORMAT, "an entry of files that is not an object");

  g_print ("ok - an index the format does not describe is refused\n");
}

static void
test_index_paths_are_refused (void)
{
  /* A path that steps out of the instance. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Escape\",\"files\":["
    "{\"path\":\"mods/../../escape.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/escape.jar\"]}],"
    "\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_PATH, "a path that steps out of the instance");

  /* An absolute path. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Absolute\",\"files\":["
    "{\"path\":\"/etc/escape.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/escape.jar\"]}],"
    "\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_PATH, "an absolute path");

  /* A path with the separator of another system. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Backslash\",\"files\":["
    "{\"path\":\"mods\\\\escape.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/escape.jar\"]}],"
    "\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_PATH, "a path with a backslash in it");

  /* A drive name. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Drive\",\"files\":["
    "{\"path\":\"C:/escape.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/escape.jar\"]}],"
    "\"dependencies\":{}}",
    nuvasha_MODPACK_ERROR_PATH, "a path with a drive name in it");

  g_print ("ok - a path the instance cannot hold is refused\n");
}

static void
test_broken_json_is_refused (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;

  /* Not JSON at all: the parser says so, in its own words. */
  pack = nuvasha_modpack_new_from_json ("this is not json", -1, &error);
  g_assert_null (pack);
  g_assert_nonnull (error);

  g_print ("ok - bytes that are not JSON are refused\n");
}

/* ---------------------------------------------------------- *
 * What the two hashes of a file prove
 * ---------------------------------------------------------- */

static void
test_verify_hashes (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "mod.jar", NULL);
  g_autoptr (GError) error = NULL;
  const char payload[] = "the bytes of a mod jar are not these";
  g_autofree char *sha1 = g_compute_checksum_for_data (
    G_CHECKSUM_SHA1, (const guchar *) payload, strlen (payload));
  g_autofree char *sha512 = g_compute_checksum_for_data (
    G_CHECKSUM_SHA512, (const guchar *) payload, strlen (payload));
  g_autofree char *other_sha1 = g_compute_checksum_for_data (
    G_CHECKSUM_SHA1, (const guchar *) "something else", 14);
  NuvashaModpackFile file = { 0 };

  g_assert_true (g_file_set_contents (path, payload, -1, &error));
  g_assert_no_error (error);

  file.path = "mods/mod.jar";
  file.sha1 = sha1;
  file.sha512 = sha512;

  /* What the manifest says is what is on disk. */
  g_assert_true (verify_now (&file, path, &error));
  g_assert_no_error (error);

  /* A sha1 that does not match. */
  file.sha1 = other_sha1;
  g_assert_false (verify_now (&file, path, &error));
  g_assert_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_HASH);
  g_assert_nonnull (strstr (error->message, "sha1"));
  g_clear_error (&error);

  /* A sha512 that does not match. */
  file.sha1 = sha1;
  file.sha512 = "000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000";
  g_assert_false (verify_now (&file, path, &error));
  g_assert_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_HASH);
  g_assert_nonnull (strstr (error->message, "sha512"));
  g_clear_error (&error);

  remove_recursive (dir);
  g_print ("ok - the two hashes of the manifest are both checked\n");
}

/* ---------------------------------------------------------- *
 * What an install leaves behind
 * ---------------------------------------------------------- */

static NuvashaInstanceManager *
manager_new (const char *data_dir)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager = nuvasha_instance_manager_new (data_dir);

  g_assert_nonnull (manager);
  g_assert_true (nuvasha_instance_manager_load (manager, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (manager_count (manager), ==, 0);

  return g_steal_pointer (&manager);
}

static void
test_install_offline (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *index_json =
    pack_index ("Offline Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autofree char *instances_dir = g_build_filename (data_dir, "instances", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;
  g_autoptr (NuvashaInstance) instance = NULL;
  g_autofree char *instance_dir = NULL;
  g_autofree char *name = NULL;
  g_autofree char *minecraft = NULL;
  g_autofree char *icon = NULL;
  NuvashaLoader loader = nuvasha_LOADER_VANILLA;
  g_autofree char *options = NULL;
  g_autofree char *client = NULL;
  g_autofree char *server = NULL;

  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
    ZIP_FILE ("overrides/config/options.txt", "from overrides"),
    ZIP_FILE ("overrides/config/mods.toml", "only overrides"),
    ZIP_FILE ("overrides/icon.png", "icon bytes"),
    ZIP_FILE ("overrides/mods/.index/index.json", "internal index"),
    ZIP_FILE ("client-overrides/config/options.txt", "from client"),
    ZIP_FILE ("server-overrides/config/server.txt", "from server"),
  };

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));

  manager = manager_new (data_dir);

  pack = nuvasha_modpack_new_from_zip (zip_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_cmpstr (pack->archive_path, ==, zip_path);
  g_assert_true (nuvasha_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  /* No directory of its own: the manager's default for the name. */
  instance = install_now (pack, manager, "Offline Pack", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_object_get (instance, "directory", &instance_dir, "name", &name,
                "minecraft-version", &minecraft, "loader", &loader,
                "icon", &icon, NULL);
  g_assert_cmpstr (name, ==, "Offline Pack");
  g_assert_cmpstr (minecraft, ==, "1.20.1");
  g_assert_cmpint (loader, ==, nuvasha_LOADER_VANILLA);
  g_assert_true (g_str_has_prefix (instance_dir, instances_dir));
  g_assert_true (g_file_test (instance_dir, G_FILE_TEST_IS_DIR));
  g_assert_nonnull (icon);
  {
    g_autofree char *icon_name = g_path_get_basename (icon);
    g_autofree char *icon_data = read_file (icon);

    g_assert_cmpstr (icon_name, ==, "icon.png");
    g_assert_cmpstr (icon_data, ==, "icon bytes");
  }
  {
    g_autofree char *imported_index =
      g_build_filename (instance_dir, "mods", ".index", "index.json", NULL);

    g_assert_false (g_file_test (imported_index, G_FILE_TEST_EXISTS));
  }

  /* overrides lands, client-overrides lands on top of it, and
   * server-overrides has nothing to do on the client. */
  {
    g_autofree char *options_path =
      g_build_filename (instance_dir, "config", "options.txt", NULL);
    g_autofree char *mods_path =
      g_build_filename (instance_dir, "config", "mods.toml", NULL);
    g_autofree char *server_path =
      g_build_filename (instance_dir, "config", "server.txt", NULL);

    options = read_file (options_path);
    g_assert_cmpstr (options, ==, "from client");

    client = read_file (mods_path);
    g_assert_cmpstr (client, ==, "only overrides");

    server = g_steal_pointer (&server_path);
    g_assert_false (g_file_test (server, G_FILE_TEST_EXISTS));
  }

  /* The instance is registered, and no temporary directory is left
   * next to it. */
  g_assert_cmpuint (manager_count (manager), ==, 1);
  g_assert_false (has_temp_dir (instances_dir));

  remove_recursive (dir);
  g_print ("ok - a pack with no files installs from its overrides\n");
}

static void
test_install_rolls_back (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *target =
    g_build_filename (data_dir, "instances", "rollback", NULL);
  g_autofree char *parent = g_path_get_dirname (target);
  g_autofree char *files_json = g_strdup_printf (
    "{\"path\":\"mods/x.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"fileSize\":10,"
    "\"downloads\":[\"https://evil.example/x.jar\"]}");
  g_autofree char *index_json =
    pack_index ("Bad Pack", files_json, "\"minecraft\":\"1.20.1\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;
  g_autoptr (NuvashaInstance) instance = NULL;

  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
  };

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));

  manager = manager_new (data_dir);

  pack = nuvasha_modpack_new_from_zip (zip_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvasha_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  instance = install_now (pack, manager, "Bad Pack", target, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_URL);
  g_clear_error (&error);

  /* Nothing of the pack is where it wanted to go, nothing of it is
   * next to where it wanted to go, and nothing lists an instance. */
  g_assert_false (g_file_test (target, G_FILE_TEST_EXISTS));
  g_assert_false (has_temp_dir (parent));
  g_assert_cmpuint (manager_count (manager), ==, 0);

  remove_recursive (dir);
  g_print ("ok - a pack whose URLs are refused leaves nothing behind\n");
}

static void
test_install_leaves_user_data (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *target =
    g_build_filename (data_dir, "instances", "taken", NULL);
  g_autofree char *parent = g_path_get_dirname (target);
  g_autofree char *world = g_build_filename (target, "world", "level.dat", NULL);
  g_autofree char *index_json =
    pack_index ("Taken Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;
  g_autoptr (NuvashaInstance) instance = NULL;

  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
  };

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));

  /* What is in the place already is user data, not ours to touch. */
  g_autofree char *world_dir = g_path_get_dirname (world);

  g_assert_cmpint (g_mkdir_with_parents (world_dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (world, "precious", -1, &error));
  g_assert_no_error (error);

  manager = manager_new (data_dir);

  pack = nuvasha_modpack_new_from_zip (zip_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvasha_modpack_resolve (pack, &error));
  g_assert_no_error (error);

  instance = install_now (pack, manager, "Taken Pack", target, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_IO);
  g_clear_error (&error);

  /* The user data is as it was, and no temporary directory was even
   * made: the refusal happens in front of any tree. */
  g_autofree char *kept = read_file (world);

  g_assert_cmpstr (kept, ==, "precious");
  g_assert_false (has_temp_dir (parent));
  g_assert_cmpuint (manager_count (manager), ==, 0);

  remove_recursive (dir);
  g_print ("ok - a place that is taken is left as it is\n");
}

static void
test_install_needs_resolve (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *index_json =
    pack_index ("Unread Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;
  g_autoptr (NuvashaInstance) instance = NULL;

  manager = manager_new (data_dir);

  pack = nuvasha_modpack_new_from_json (index_json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);

  /* The dependencies were never read: the install says so instead of
   * guessing a Minecraft version. */
  instance = install_now (pack, manager, "Unread Pack", NULL, &error);
  g_assert_null (instance);
  g_assert_error (error, nuvasha_MODPACK_ERROR, nuvasha_MODPACK_ERROR_LOADER);
  g_clear_error (&error);

  g_assert_cmpuint (manager_count (manager), ==, 0);

  remove_recursive (dir);
  g_print ("ok - an install whose dependencies were never read is refused\n");
}

static void
test_export_import_round_trip (void)
{
  g_autofree char *root = make_tmp_dir ();
  g_autofree char *source_data = g_build_filename (root, "source-data", NULL);
  g_autofree char *target_data = g_build_filename (root, "target-data", NULL);
  g_autofree char *source_dir = g_build_filename (root, "source-instance", NULL);
  g_autofree char *icon_source = g_build_filename (root, "source-icon.png", NULL);
  g_autofree char *archive = g_build_filename (root, "round-trip.mrpack", NULL);
  g_autofree char *override = g_build_filename (source_dir, "config", "test.txt", NULL);
  g_autofree char *log = g_build_filename (source_dir, "logs", "latest.log", NULL);
  g_autofree char *index =
    g_build_filename (source_dir, "mods", ".index", "index.json", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstanceManager) source_manager = manager_new (source_data);
  g_autoptr (NuvashaInstanceManager) target_manager = manager_new (target_data);
  g_autoptr (NuvashaInstance) source = NULL;
  g_autoptr (NuvashaModpack) pack = NULL;
  g_autoptr (NuvashaInstance) imported = NULL;
  g_autofree char *imported_dir = NULL;
  g_autofree char *imported_icon = NULL;
  g_autofree char *icon_bytes = NULL;
  g_autofree char *override_bytes = NULL;
  NuvashaLoader loader = nuvasha_LOADER_VANILLA;

  source = nuvasha_instance_manager_create (source_manager, "Round Trip",
                                        source_dir, "1.20.1", nuvasha_LOADER_FABRIC,
                                        "0.15.11", &error);
  g_assert_no_error (error);
  g_assert_nonnull (source);
  g_assert_true (g_file_set_contents (icon_source, "icon bytes", -1, &error));
  g_assert_no_error (error);
  {
    g_autofree char *override_dir = g_path_get_dirname (override);

    g_assert_cmpint (g_mkdir_with_parents (override_dir, 0755), ==, 0);
  }
  g_assert_true (g_file_set_contents (override, "override bytes", -1, &error));
  g_assert_no_error (error);
  {
    g_autofree char *log_dir = g_path_get_dirname (log);

    g_assert_cmpint (g_mkdir_with_parents (log_dir, 0755), ==, 0);
  }
  g_assert_true (g_file_set_contents (log, "transient bytes", -1, &error));
  g_assert_no_error (error);
  {
    g_autofree char *index_dir = g_path_get_dirname (index);

    g_assert_cmpint (g_mkdir_with_parents (index_dir, 0755), ==, 0);
  }
  g_assert_true (g_file_set_contents (index, "internal index", -1, &error));
  g_assert_no_error (error);
  g_object_set (source, "icon", icon_source, NULL);
  g_assert_true (nuvasha_instance_manager_save (source_manager, &error));
  g_assert_no_error (error);

  g_assert_true (export_now (source, archive, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (archive, G_FILE_TEST_IS_REGULAR));

  {
    struct archive *reader = archive_read_new ();
    struct archive_entry *entry = NULL;
    guint icon_entries = 0;

    g_assert_cmpint (archive_read_support_format_zip (reader), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_read_open_filename (reader, archive, 10240), ==,
                     ARCHIVE_OK);
    while (archive_read_next_header (reader, &entry) == ARCHIVE_OK)
      if (g_strcmp0 (archive_entry_pathname (entry), "overrides/icon.png") ==
          0)
        icon_entries++;
    g_assert_cmpint (archive_read_free (reader), ==, ARCHIVE_OK);
    g_assert_cmpuint (icon_entries, ==, 1);
  }

  pack = nuvasha_modpack_new_from_zip (archive, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvasha_modpack_resolve (pack, &error));
  g_assert_no_error (error);
  g_assert_cmpint (pack->loader, ==, nuvasha_LOADER_FABRIC);

  /* The manifest loader is what this round trip verifies. Avoid asking the
   * live Fabric service during tests; the actual files, icon and overrides
   * are independent of the loader installer lookup. */
  pack->loader = nuvasha_LOADER_VANILLA;
  g_clear_pointer (&pack->loader_version, g_free);

  imported = install_now (pack, target_manager, "Round Trip", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (imported);
  g_object_get (imported, "loader", &loader, "directory", &imported_dir,
                "icon", &imported_icon, NULL);
  g_assert_cmpint (loader, ==, nuvasha_LOADER_VANILLA);
  g_assert_nonnull (imported_icon);
  icon_bytes = read_file (imported_icon);
  g_assert_cmpstr (icon_bytes, ==, "icon bytes");

  {
    g_autofree char *imported_override =
      g_build_filename (imported_dir, "config", "test.txt", NULL);
    override_bytes = read_file (imported_override);
  }
  g_assert_cmpstr (override_bytes, ==, "override bytes");
  {
    g_autofree char *imported_log =
      g_build_filename (imported_dir, "logs", "latest.log", NULL);
    g_autofree char *imported_index =
      g_build_filename (imported_dir, "mods", ".index", "index.json", NULL);

    g_assert_false (g_file_test (imported_log, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (imported_index, G_FILE_TEST_EXISTS));
  }

  remove_recursive (root);
  g_print ("ok - export and import preserve loader, icon and overrides\n");
}

/* ---------------------------------------------------------- *
 * The dialog's own flow
 *
 * Reading a pack and installing it through the dialog is what a user does,
 * and it is a different path from the install this file otherwise tests:
 * the dialog hands its own references around and releases the operations
 * it started. Local only, so it runs without a network. The widgets need
 * a display; without one the flow is skipped rather than faked. */

static void
test_dialog_imports_and_installs (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *index_json =
    pack_index ("Dialog Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  g_autoptr (NuvashaDownloadModpackDialog) dialog = NULL;
  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
    ZIP_FILE ("overrides/config/options.txt", "from overrides"),
    ZIP_FILE ("overrides/icon.png", "icon bytes"),
  };
  gboolean finished = FALSE;
  gint64 deadline = 0;

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));
  manager = manager_new (data_dir);

  dialog = nuvasha_download_modpack_dialog_new_for_file (manager, zip_path);
  g_assert_nonnull (dialog);
  /* The constructor returns a floating dialog, as GTK widget constructors
   * do. Keep the reference owned by this test rather than relying on an
   * unrelated in-flight browser request to keep the dialog alive. */
  g_object_ref_sink (dialog);

  /* The import runs on the main loop: the pack is read, the install goes
     on, and both release the operations they started. */
  deadline = g_get_monotonic_time () + 30 * G_TIME_SPAN_SECOND;

  while (!finished && g_get_monotonic_time () < deadline)
    {
      if (manager_count (manager) > 0)
        finished = TRUE;

      g_main_context_iteration (NULL, FALSE);
    }

  g_assert_true (finished);
  g_assert_cmpuint (manager_count (manager), ==, 1);

  remove_recursive (dir);
  g_print ("ok - the dialog reads a pack and installs it in one go\n");
}

/* ---------------------------------------------------------- */

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/modpack/index", test_index_is_read);
  g_test_add_func ("/modpack/client-files", test_client_files_by_env);
  g_test_add_func ("/modpack/resolve-fabric", test_resolve_fabric);
  g_test_add_func ("/modpack/resolve-quilt", test_resolve_quilt);
  g_test_add_func ("/modpack/resolve-forge", test_resolve_forge);
  g_test_add_func ("/modpack/resolve-neoforge", test_resolve_neoforge);
  g_test_add_func ("/modpack/resolve-vanilla", test_resolve_vanilla);
  g_test_add_func ("/modpack/resolve-choice", test_resolve_loader_choice);
  g_test_add_func ("/modpack/resolve-no-minecraft",
                   test_resolve_without_minecraft);
  g_test_add_func ("/modpack/format", test_format_refusals);
  g_test_add_func ("/modpack/index-paths", test_index_paths_are_refused);
  g_test_add_func ("/modpack/broken-json", test_broken_json_is_refused);
  g_test_add_func ("/modpack/verify", test_verify_hashes);
  g_test_add_func ("/modpack/install-offline", test_install_offline);
  g_test_add_func ("/modpack/install-rollback", test_install_rolls_back);
  g_test_add_func ("/modpack/install-existing", test_install_leaves_user_data);
  g_test_add_func ("/modpack/install-unresolved", test_install_needs_resolve);
  g_test_add_func ("/modpack/export-import-round-trip",
                   test_export_import_round_trip);

  if (gtk_init_check ())
    g_test_add_func ("/modpack/dialog-import", test_dialog_imports_and_installs);

  return g_test_run ();
}
