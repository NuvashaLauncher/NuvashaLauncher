/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Consolidated pure-unit test suite for stateless modules. */

#include "nuvashalauncher-content-format.h"
#include "nuvashalauncher-json.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-nbt.h"

#include <glib.h>
#include <json-glib/json-glib.h>

static void
test_json_int_range (void)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root = NULL;

  g_assert_true (json_parser_load_from_data (
    parser, "{\"integer\":7,\"fraction\":2.5,\"huge\":1e300}", -1, NULL));
  root = json_parser_get_root (parser);
  g_assert_cmpint (nuvashalauncher_json_int (root, "integer", -1), ==, 7);
  g_assert_cmpint (nuvashalauncher_json_int (root, "fraction", -1), ==, 2);
  g_assert_cmpint (nuvashalauncher_json_int (root, "huge", 99), ==, 99);
}

static void
test_launch_state_names (void)
{
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_PREPARING), ==,
                   "Preparing");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_DOWNLOADING), ==,
                   "Downloading");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_LAUNCHING), ==,
                   "Launching");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_RUNNING), ==,
                   "Running");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_STOPPING), ==,
                   "Stopping");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_STOPPED), ==,
                   "Stopped");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_CRASHED), ==,
                   "Crashed");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_FAILED), ==,
                   "Failed");
  g_assert_cmpstr (nuvashalauncher_launch_state_to_string (NUVASHALAUNCHER_LAUNCH_NEEDS_SIGN_IN), ==,
                   "Needs sign-in");
}

static void
test_launch_state_busy (void)
{
  g_assert_true (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_PREPARING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_DOWNLOADING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_LAUNCHING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_RUNNING));
  g_assert_true (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_STOPPING));
  g_assert_false (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_STOPPED));
  g_assert_false (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_CRASHED));
  g_assert_false (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_FAILED));
  g_assert_false (nuvashalauncher_launch_state_is_busy (NUVASHALAUNCHER_LAUNCH_NEEDS_SIGN_IN));
}

static void
test_content_download_counts (void)
{
  g_autofree char *small = nuvashalauncher_content_format_downloads (42);
  g_autofree char *thousands = nuvashalauncher_content_format_downloads (1250);
  g_autofree char *millions = nuvashalauncher_content_format_downloads (2500000);

  g_assert_cmpstr (small, ==, "42 downloads");
  g_assert_cmpstr (thousands, ==, "1.2k downloads");
  g_assert_cmpstr (millions, ==, "2.5M downloads");
}

static void
test_content_author_lines (void)
{
  g_autofree char *licensed =
    nuvashalauncher_content_format_author ("MIT", "Example Author");
  g_autofree char *author_only =
    nuvashalauncher_content_format_author (NULL, "Example Author");
  g_autofree char *empty_author = nuvashalauncher_content_format_author ("MIT", NULL);

  g_assert_cmpstr (licensed, ==, "MIT  ·  by Example Author");
  g_assert_cmpstr (author_only, ==, "by Example Author");
  g_assert_cmpstr (empty_author, ==, "MIT  ·  by ");
}

static void
test_desktop_entry (void)
{
  g_autofree gchar *path =
    g_build_filename (NUVASHALAUNCHER_DATA_DIR,
                      "com.github.NuvashaLauncher.NuvashaLauncher.desktop", NULL);
  g_autoptr (GKeyFile) keys = g_key_file_new ();
  g_autoptr (GError) error = NULL;
  g_autofree gchar *type = NULL;
  g_autofree gchar *name = NULL;
  g_autofree gchar *comment = NULL;
  g_autofree gchar *exec = NULL;
  g_autofree gchar *icon = NULL;
  g_autofree gchar *mime_type = NULL;
  g_auto (GStrv) categories = NULL;

  g_key_file_load_from_file (keys, path, G_KEY_FILE_NONE, &error);
  g_assert_no_error (error);

  type = g_key_file_get_string (keys, "Desktop Entry", "Type", NULL);
  g_assert_cmpstr (type, ==, "Application");

  name = g_key_file_get_string (keys, "Desktop Entry", "Name", NULL);
  g_assert_cmpstr (name, ==, "Nuvasha Launcher");

  comment = g_key_file_get_string (keys, "Desktop Entry", "Comment", NULL);
  g_assert_cmpstr (comment, ==, "Minecraft launcher for Linux");

  exec = g_key_file_get_string (keys, "Desktop Entry", "Exec", NULL);
  g_assert_cmpstr (exec, ==, "nuvashalauncher %f");

  mime_type = g_key_file_get_string (keys, "Desktop Entry", "MimeType", NULL);
  g_assert_cmpstr (mime_type, ==, "application/x-modrinth-modpack;");

  icon = g_key_file_get_string (keys, "Desktop Entry", "Icon", NULL);
  g_assert_cmpstr (icon, ==, "com.github.NuvashaLauncher.NuvashaLauncher");

  g_assert_true (g_key_file_has_key (keys, "Desktop Entry", "Terminal", NULL));
  g_assert_false (g_key_file_get_boolean (keys, "Desktop Entry", "Terminal", NULL));

  categories = g_key_file_get_string_list (keys, "Desktop Entry", "Categories", NULL, NULL);
  g_assert_nonnull (categories);
  g_assert_cmpstr (categories[0], ==, "Game");

  g_assert_false (g_key_file_has_key (keys, "Desktop Entry", "StartupWMClass", NULL));
}

static void
test_nbt_roundtrip_uncompressed (void)
{
  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "");
  g_autoptr (NuvashaLauncherNbtTag) list =
    nuvashalauncher_nbt_new_list ("servers", NUVASHALAUNCHER_NBT_COMPOUND);
  NuvashaLauncherNbtTag *entry =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, NULL);

  nuvashalauncher_nbt_compound_set (entry, nuvashalauncher_nbt_new_string ("name", "My Server"));
  nuvashalauncher_nbt_compound_set (entry, nuvashalauncher_nbt_new_string ("ip", "play.example.com"));
  nuvashalauncher_nbt_compound_set (entry, nuvashalauncher_nbt_new_byte ("acceptTextures", 1));
  g_ptr_array_add (list->v_list.items, entry);
  nuvashalauncher_nbt_compound_set (root, g_steal_pointer (&list));

  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = nuvashalauncher_nbt_serialize (root, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);
  g_autoptr (NuvashaLauncherNbtTag) parsed =
    nuvashalauncher_nbt_parse (data, len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (parsed);

  NuvashaLauncherNbtTag *servers = nuvashalauncher_nbt_compound_get (parsed, "servers");
  g_assert_nonnull (servers);
  g_assert_cmpint (servers->type, ==, NUVASHALAUNCHER_NBT_LIST);
  g_assert_cmpuint (servers->v_list.items->len, ==, 1);

  NuvashaLauncherNbtTag *first = g_ptr_array_index (servers->v_list.items, 0);
  g_assert_cmpstr (nuvashalauncher_nbt_compound_get_string (first, "name"), ==, "My Server");
  g_assert_cmpstr (nuvashalauncher_nbt_compound_get_string (first, "ip"), ==, "play.example.com");
  g_assert_cmpint (nuvashalauncher_nbt_compound_get_byte (first, "acceptTextures", 0), ==, 1);
}

static void
test_nbt_roundtrip_gzip (void)
{
  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "");
  NuvashaLauncherNbtTag *data_tag =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "Data");

  nuvashalauncher_nbt_compound_set (data_tag, nuvashalauncher_nbt_new_string ("LevelName", "Survival World"));
  nuvashalauncher_nbt_compound_set (data_tag, nuvashalauncher_nbt_new_int ("GameType", 0));
  nuvashalauncher_nbt_compound_set (data_tag, nuvashalauncher_nbt_new_long ("RandomSeed", 1234567890123LL));
  nuvashalauncher_nbt_compound_set (root, data_tag);

  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = nuvashalauncher_nbt_serialize (root, TRUE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);

  gsize len = 0;
  const guint8 *raw = g_bytes_get_data (bytes, &len);
  g_assert_cmpuint (len, >=, 2);
  g_assert_cmpuint (raw[0], ==, 0x1f);
  g_assert_cmpuint (raw[1], ==, 0x8b);

  g_autoptr (NuvashaLauncherNbtTag) parsed =
    nuvashalauncher_nbt_parse (raw, len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (parsed);

  NuvashaLauncherNbtTag *d = nuvashalauncher_nbt_compound_get (parsed, "Data");
  g_assert_nonnull (d);
  g_assert_cmpstr (nuvashalauncher_nbt_compound_get_string (d, "LevelName"), ==, "Survival World");
  g_assert_cmpint (nuvashalauncher_nbt_compound_get_int (d, "GameType", -1), ==, 0);
  g_assert_cmpint (nuvashalauncher_nbt_compound_get_long (d, "RandomSeed", 0), ==, 1234567890123LL);
}

static void
test_mc_services_message (void)
{
  g_autoptr (SoupMessage) message =
    nuvashalauncher_minecraft_services_message (SOUP_METHOD_GET, "/minecraft/profile", "token");
  SoupMessageHeaders *headers = soup_message_get_request_headers (message);

  g_assert_nonnull (message);
  g_assert_cmpstr (g_uri_get_path (soup_message_get_uri (message)), ==, "/minecraft/profile");
  g_assert_cmpstr (soup_message_headers_get_one (headers, "Accept"), ==, "application/json");
  g_assert_cmpstr (soup_message_headers_get_one (headers, "Authorization"), ==, "Bearer token");
}

static void
test_mc_services_retry_policy (void)
{
  g_autoptr (SoupMessage) message = soup_message_new (SOUP_METHOD_GET, "https://example.invalid");
  SoupMessageHeaders *headers = soup_message_get_response_headers (message);

  g_assert_cmpuint (nuvashalauncher_minecraft_services_retry_delay (message, 0), ==, 1);
  soup_message_headers_replace (headers, "Retry-After", "23");
  g_assert_cmpuint (nuvashalauncher_minecraft_services_retry_delay (message, 0), ==, 23);
  soup_message_headers_remove (headers, "Retry-After");
  g_assert_cmpuint (nuvashalauncher_minecraft_services_retry_delay (message, 8), ==, 120);
}

static void
test_mc_services_error_domain (void)
{
  g_autoptr (GError) error =
    nuvashalauncher_minecraft_services_response_error (NULL, NULL);

  g_assert_error (error, NUVASHALAUNCHER_MINECRAFT_SERVICES_ERROR,
                  NUVASHALAUNCHER_MINECRAFT_SERVICES_ERROR_HTTP);
}

static void
test_mc_services_skin_multipart (void)
{
  g_autoptr (GBytes) skin = g_bytes_new_static ("PNG", 3);
  g_autofree char *content_type = NULL;
  g_autoptr (GBytes) body =
    nuvashalauncher_minecraft_services_skin_multipart (skin, "SLIM", &content_type);
  gsize length = 0;
  const char *data = g_bytes_get_data (body, &length);

  g_assert_true (g_str_has_prefix (content_type, "multipart/form-data; boundary="));
  g_assert_cmpint (length, >, 3);
  g_assert_nonnull (g_strstr_len (data, length, "name=\"file\""));
  g_assert_nonnull (g_strstr_len (data, length, "name=\"variant\""));
  g_assert_nonnull (g_strstr_len (data, length, "SLIM"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/unit/json/int-range", test_json_int_range);
  g_test_add_func ("/unit/launch-state/names", test_launch_state_names);
  g_test_add_func ("/unit/launch-state/busy", test_launch_state_busy);
  g_test_add_func ("/unit/content-format/downloads", test_content_download_counts);
  g_test_add_func ("/unit/content-format/author", test_content_author_lines);
  g_test_add_func ("/unit/desktop-entry/valid", test_desktop_entry);
  g_test_add_func ("/unit/nbt/roundtrip-uncompressed", test_nbt_roundtrip_uncompressed);
  g_test_add_func ("/unit/nbt/roundtrip-gzip", test_nbt_roundtrip_gzip);
  g_test_add_func ("/unit/minecraft-services/message", test_mc_services_message);
  g_test_add_func ("/unit/minecraft-services/retry-policy", test_mc_services_retry_policy);
  g_test_add_func ("/unit/minecraft-services/error-domain", test_mc_services_error_domain);
  g_test_add_func ("/unit/minecraft-services/skin-multipart", test_mc_services_skin_multipart);
  return g_test_run ();
}
