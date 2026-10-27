#include "test-modpack-common.h"

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
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  nuvashalauncherModpackFile *first = NULL;
  nuvashalauncherModpackFile *last = NULL;

  pack = nuvashalauncher_modpack_new_from_json (json, -1, &error);
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
  g_assert_cmpint (first->env_client, ==, nuvashalauncher_MODPACK_ENV_REQUIRED);
  g_assert_cmpint (first->env_server, ==, nuvashalauncher_MODPACK_ENV_UNSUPPORTED);
  g_assert_cmpuint (first->downloads->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (first->downloads, 0), ==,
                   "https://cdn.modrinth.com/data/aa/v1/sodium.jar");

  /* The last file has no env object: what both sides can run. */
  last = g_ptr_array_index (pack->files, 3);
  g_assert_cmpstr (last->path, ==, "config/both.toml");
  g_assert_cmpint (last->env_client, ==, nuvashalauncher_MODPACK_ENV_UNSPECIFIED);
  g_assert_cmpint (last->env_server, ==, nuvashalauncher_MODPACK_ENV_UNSPECIFIED);
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
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  g_autoptr (GPtrArray) required = NULL;
  g_autoptr (GPtrArray) optional = NULL;

  pack = nuvashalauncher_modpack_new_from_json (json, -1, &error);
  g_assert_no_error (error);

  required = nuvashalauncher_modpack_required_client_files (pack);
  optional = nuvashalauncher_modpack_optional_client_files (pack);

  g_assert_cmpuint (required->len, ==, 2);
  g_assert_cmpstr (((nuvashalauncherModpackFile *) g_ptr_array_index (required, 0))->path,
                   ==, "mods/sodium.jar");
  g_assert_cmpstr (((nuvashalauncherModpackFile *) g_ptr_array_index (required, 1))->path,
                   ==, "config/both.toml");

  g_assert_cmpuint (optional->len, ==, 1);
  g_assert_cmpstr (((nuvashalauncherModpackFile *) g_ptr_array_index (optional, 0))->path,
                   ==, "mods/lithium.jar");

  g_print ("ok - the env of a file says which list it belongs to\n");
}

static void
test_resolve_fabric (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"fabric-loader\":\"0.15.11\"",
                 nuvashalauncher_LOADER_FABRIC, "1.20.1", "0.15.11");
  g_print ("ok - a pack that names fabric-loader resolves to fabric\n");
}

static void
test_resolve_quilt (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"quilt-loader\":\"0.21.0\"",
                 nuvashalauncher_LOADER_QUILT, "1.20.1", "0.21.0");
  g_print ("ok - a pack that names quilt-loader resolves to quilt\n");
}

static void
test_resolve_forge (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"forge\":\"47.2.0\"",
                 nuvashalauncher_LOADER_FORGE, "1.20.1", "47.2.0");
  g_print ("ok - a pack that names forge resolves to forge\n");
}

static void
test_resolve_neoforge (void)
{
  assert_loader ("\"minecraft\":\"1.20.1\",\"neoforge\":\"20.4.0\"",
                 nuvashalauncher_LOADER_NEOFORGE, "1.20.1", "20.4.0");
  g_print ("ok - a pack that names neoforge resolves to neoforge\n");
}

static void
test_resolve_vanilla (void)
{
  /* A pack that names no loader runs on vanilla, and a name the
   * format does not know is ignored rather than refused. */
  assert_loader ("\"minecraft\":\"1.21\",\"some-future-loader\":\"9.9\"",
                 nuvashalauncher_LOADER_VANILLA, "1.21", NULL);
  g_print ("ok - a pack that names no loader resolves to vanilla\n");
}

static void
test_resolve_loader_choice (void)
{
  /* Two loaders named: the one the format puts first wins. */
  assert_loader ("\"minecraft\":\"1.20.1\",\"forge\":\"47.2.0\","
                 "\"fabric-loader\":\"0.15.11\"",
                 nuvashalauncher_LOADER_FABRIC, "1.20.1", "0.15.11");
  g_print ("ok - of two loaders named, the first of the format wins\n");
}

static void
test_resolve_without_minecraft (void)
{
  g_autofree char *json =
    pack_index ("No Minecraft", "", "\"fabric-loader\":\"0.15.11\"");
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;

  pack = nuvashalauncher_modpack_new_from_json (json, -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_false (nuvashalauncher_modpack_resolve (pack, &error));
  g_assert_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_LOADER);

  g_print ("ok - a pack that names no Minecraft version is refused\n");
}

static void
test_format_refusals (void)
{
  /* A format version other than the one the format has today. */
  expect_refused (
    "{\"formatVersion\":2,\"game\":\"minecraft\",\"versionId\":\"2\","
    "\"name\":\"Too New\",\"files\":[],\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_FORMAT, "a format version nuvashalauncher does not know");

  /* A pack for another game. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"dwarf-fortress\",\"versionId\":\"1\","
    "\"name\":\"Not Minecraft\",\"files\":[],\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_FORMAT, "a pack for another game");

  /* A file with one hash missing. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Half Hashed\",\"files\":["
    "{\"path\":\"mods/x.jar\",\"hashes\":{\"sha1\":\"aa\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/x.jar\"]}],"
    "\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_FORMAT, "a file with one hash missing");

  /* No files array at all. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"No Files\",\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_FORMAT, "an index without a files array");

  /* No name. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"files\":[],\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_FORMAT, "an index without a name");

  /* A files member that is not an array. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Odd Files\",\"files\":{},\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_FORMAT, "a files member that is not an array");

  /* An entry of files that is not an object. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Odd Entry\",\"files\":[\"mods/x.jar\"],\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_FORMAT, "an entry of files that is not an object");

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
    nuvashalauncher_MODPACK_ERROR_PATH, "a path that steps out of the instance");

  /* An absolute path. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Absolute\",\"files\":["
    "{\"path\":\"/etc/escape.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/escape.jar\"]}],"
    "\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_PATH, "an absolute path");

  /* A path with the separator of another system. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Backslash\",\"files\":["
    "{\"path\":\"mods\\\\escape.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/escape.jar\"]}],"
    "\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_PATH, "a path with a backslash in it");

  /* A drive name. */
  expect_refused (
    "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"1\","
    "\"name\":\"Drive\",\"files\":["
    "{\"path\":\"C:/escape.jar\","
    "\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
    "\"downloads\":[\"https://cdn.modrinth.com/escape.jar\"]}],"
    "\"dependencies\":{}}",
    nuvashalauncher_MODPACK_ERROR_PATH, "a path with a drive name in it");

  g_print ("ok - a path the instance cannot hold is refused\n");
}

static void
test_broken_json_is_refused (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;

  /* Not JSON at all: the parser says so, in its own words. */
  pack = nuvashalauncher_modpack_new_from_json ("this is not json", -1, &error);
  g_assert_null (pack);
  g_assert_nonnull (error);

  g_print ("ok - bytes that are not JSON are refused\n");
}

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
  nuvashalauncherModpackFile file = { 0 };

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
  g_assert_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_HASH);
  g_assert_nonnull (strstr (error->message, "sha1"));
  g_clear_error (&error);

  /* A sha512 that does not match. */
  file.sha1 = sha1;
  file.sha512 = "000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000";
  g_assert_false (verify_now (&file, path, &error));
  g_assert_error (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_HASH);
  g_assert_nonnull (strstr (error->message, "sha512"));
  g_clear_error (&error);

  remove_recursive (dir);
  g_print ("ok - the two hashes of the manifest are both checked\n");
}

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
  g_test_add_func ("/modpack/resolve-no-minecraft", test_resolve_without_minecraft);
  g_test_add_func ("/modpack/format", test_format_refusals);
  g_test_add_func ("/modpack/index-paths", test_index_paths_are_refused);
  g_test_add_func ("/modpack/broken-json", test_broken_json_is_refused);
  g_test_add_func ("/modpack/verify", test_verify_hashes);
  return g_test_run ();
}
