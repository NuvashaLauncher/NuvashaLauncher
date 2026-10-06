/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for the parts of aml-launcher that are only functions: the
 * offline UUID, the substitution of the placeholders a version
 * document writes, the command line a launch builds from a modern and
 * from a legacy document, and the printable form of a command line with
 * the token taken out of it.
 *
 * No JVM, no network and no display are involved: the resolved version
 * the argument builder takes is written here by hand, which is the same
 * shape aml_resolve_version_finish() hands back. */

#include "aml-launcher.h"

#include "aml-minecraft-resolve.h"

#include <glib.h>
#include <stdlib.h>

/* ---------------------------------------------------------- *
 * Helpers
 * ---------------------------------------------------------- */

static GPtrArray *
strings (const char *first, ...)
{
  GPtrArray *array = g_ptr_array_new_with_free_func (g_free);
  va_list args;
  const char *one;

  va_start (args, first);

  for (one = first; one != NULL; one = va_arg (args, const char *))
    g_ptr_array_add (array, g_strdup (one));

  va_end (args);

  return array;
}

/* A placeholder table out of name and value pairs, ended by NULL. */
static GHashTable *
table (const char *first, ...)
{
  GHashTable *placeholders = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, g_free);
  va_list args;
  const char *name;

  va_start (args, first);

  for (name = first; name != NULL; name = va_arg (args, const char *))
    g_hash_table_insert (placeholders, g_strdup (name),
                         g_strdup (va_arg (args, const char *)));

  va_end (args);

  return placeholders;
}

static char *
at (GPtrArray *argv, guint index)
{
  g_assert_cmpuint (index, <, argv->len);

  return g_ptr_array_index (argv, index);
}

/* A resolved version with everything filled in, the way the resolve
 * module hands it over after substituting what it knows. */
static AmlResolvedVersion *
resolved_new (const char *version_id)
{
  AmlResolvedVersion *resolved = g_new0 (AmlResolvedVersion, 1);

  resolved->version_id = g_strdup (version_id);
  resolved->client_jar = g_strdup ("/home/u/.local/share/AML/versions/1.20.1/"
                                   "1.20.1.jar");
  resolved->classpath = g_strdup ("/home/u/.local/share/AML/libraries/lwjgl.jar:"
                                  "/home/u/.local/share/AML/versions/1.20.1/"
                                  "1.20.1.jar");
  resolved->natives_directory =
    g_strdup ("/home/u/.local/share/AML/natives");
  resolved->assets_directory = g_strdup ("/home/u/.local/share/AML/assets");
  resolved->assets_index_name = g_strdup ("12");
  resolved->main_class = g_strdup ("net.minecraft.client.main.Main");
  resolved->library_directory =
    g_strdup ("/home/u/.local/share/AML/libraries");
  resolved->java_major_version = 17;

  return resolved;
}

/* ---------------------------------------------------------- *
 * The offline UUID
 * ---------------------------------------------------------- */

static void
test_offline_uuid (void)
{
  g_autofree char *notch = aml_launch_offline_uuid ("Notch");
  g_autofree char *player = aml_launch_offline_uuid (AML_LAUNCH_OFFLINE_NAME);
  g_autofree char *player_again =
    aml_launch_offline_uuid (AML_LAUNCH_OFFLINE_NAME);

  /* What java.util.UUID.nameUUIDFromBytes() computes for
     "OfflinePlayer:<name>", which is how a launcher derives the id of an
     offline player. The value for Notch is the one every offline player
     list has carried for years. */
  g_assert_cmpstr (notch, ==,
                   "b50ad385-829d-3141-a216-7e7d7539ba7f");

  g_assert_cmpstr (player, ==,
                   "a01e3843-e521-3998-958a-f459800e4d11");
  g_assert_cmpstr (player_again, ==, player);

  /* The shape of a name based UUID: version 3 and the IETF variant. */
  g_assert_cmpint (player[14], ==, '3');
  g_assert_true (g_str_has_prefix (player + 19, "8") ||
                 g_str_has_prefix (player + 19, "9") ||
                 g_str_has_prefix (player + 19, "a") ||
                 g_str_has_prefix (player + 19, "b"));

  /* A different name is a different UUID. */
  {
    g_autofree char *flia = aml_launch_offline_uuid ("Flia");

    g_assert_cmpstr (flia, ==, "41b05633-42e0-3a6f-b64b-6b64448dccdd");
  }
}

/* ---------------------------------------------------------- *
 * Placeholders
 * ---------------------------------------------------------- */

static void
test_substitute_known_and_unknown (void)
{
  g_autoptr (GHashTable) placeholders =
    table ("auth_player_name", "Player", "game_directory", "/games/x", NULL);
  g_autofree char *whole = aml_launch_substitute (
    "--username ${auth_player_name} --gameDir ${game_directory}",
    placeholders);
  g_autofree char *partial =
    aml_launch_substitute ("-Dx=${auth_player_name}/extra", placeholders);
  g_autofree char *unknown =
    aml_launch_substitute ("--x ${not_a_placeholder} y", placeholders);
  g_autofree char *empty = aml_launch_substitute ("nothing here",
                                                  placeholders);
  g_autofree char *unclosed = aml_launch_substitute ("${unclosed",
                                                      placeholders);

  g_assert_cmpstr (whole, ==,
                   "--username Player --gameDir /games/x");
  g_assert_cmpstr (partial, ==, "-Dx=Player/extra");

  /* A name the table does not hold stays as the document wrote it: a
     launcher that does not know a placeholder must not drop it. */
  g_assert_cmpstr (unknown, ==, "--x ${not_a_placeholder} y");
  g_assert_cmpstr (empty, ==, "nothing here");
  g_assert_cmpstr (unclosed, ==, "${unclosed");
  g_assert_null (aml_launch_substitute (NULL, placeholders));
}

/* ---------------------------------------------------------- *
 * A modern document
 * ---------------------------------------------------------- */

static void
test_build_arguments_modern (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.20.1");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  guint classpaths = 0;
  guint library_paths = 0;
  g_autofree char *offline = aml_launch_offline_uuid (AML_LAUNCH_OFFLINE_NAME);

  /* What a 1.20.1 document carries: the library path and the classpath
     already, and the game arguments with their placeholders. */
  resolved->jvm_arguments =
    strings ("-Djava.library.path=${natives_directory}", "-cp",
             "${classpath}", NULL);
  resolved->game_arguments =
    strings ("--username", "${auth_player_name}", "--version",
             "${version_name}", "--gameDir", "${game_directory}",
             "--assetsDir", "${assets_root}", "--assetIndex",
             "${assets_index_name}", "--uuid", "${auth_uuid}", "--userType",
             "${user_type}", "--versionType", "${version_type}", NULL);
  resolved->logging_argument =
    g_strdup ("-Dlog4j.configurationFile=/home/u/.local/share/AML/logging/"
              "client-1.20.1.xml");

  argv = aml_launch_build_arguments (resolved, "/usr/bin/java",
                                     "/home/u/games/one", "-Xmx2G -Xss1M",
                                     &error);

  g_assert_no_error (error);
  g_assert_nonnull (argv);

  g_assert_cmpstr (at (argv, 0), ==, "/usr/bin/java");

  /* The document first, in the order it wrote them. */
  g_assert_cmpstr (at (argv, 1), ==,
                   "-Djava.library.path=/home/u/.local/share/AML/natives");
  g_assert_cmpstr (at (argv, 2), ==, "-cp");
  g_assert_cmpstr (at (argv, 3), ==, resolved->classpath);

  /* Then the instance, split like a shell line. */
  g_assert_cmpstr (at (argv, 4), ==, "-Xmx2G");
  g_assert_cmpstr (at (argv, 5), ==, "-Xss1M");

  /* A document that already carries the library path and the classpath
     does not get a second copy of either. */
  classpaths = 0;
  library_paths = 0;

  for (guint i = 0; i < argv->len; i++)
    {
      if (g_strcmp0 (at (argv, i), "-cp") == 0)
        classpaths++;

      if (g_str_has_prefix (at (argv, i), "-Djava.library.path="))
        library_paths++;
    }

  g_assert_cmpuint (classpaths, ==, 1);
  g_assert_cmpuint (library_paths, ==, 1);

  /* The command line is java, the three document arguments, the two
     instance arguments, the logging argument, the main class and the
     sixteen words of the document. */
  g_assert_cmpuint (argv->len, ==, 8 + 16);

  /* The logging argument, the main class and then the game arguments. */
  g_assert_cmpstr (at (argv, 6), ==, resolved->logging_argument);
  g_assert_cmpstr (at (argv, 7), ==, "net.minecraft.client.main.Main");
  g_assert_cmpstr (at (argv, 8), ==, "--username");
  g_assert_cmpstr (at (argv, 9), ==, AML_LAUNCH_OFFLINE_NAME);
  g_assert_cmpstr (at (argv, 11), ==, "1.20.1");
  g_assert_cmpstr (at (argv, 13), ==, "/home/u/games/one");
  g_assert_cmpstr (at (argv, 15), ==, "/home/u/.local/share/AML/assets");
  g_assert_cmpstr (at (argv, 17), ==, "12");
  g_assert_cmpstr (at (argv, 19), ==, offline);
  g_assert_cmpstr (at (argv, 21), ==, AML_LAUNCH_OFFLINE_USER_TYPE);
  g_assert_cmpstr (at (argv, 23), ==, "1.20.1");
}

/* ---------------------------------------------------------- *
 * A legacy document
 * ---------------------------------------------------------- */

static void
test_build_arguments_legacy (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.8.9");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  guint classpath = 0;
  guint main_class = 0;

  /* What a 1.8.9 document carries: no "arguments" object at all, only
     the one line of game arguments, and nothing for the JVM. */
  resolved->jvm_arguments = strings (NULL);
  resolved->game_arguments =
    strings ("--username", "${auth_player_name}", "--version",
             "${version_name}", "--gameDir", "${game_directory}", "--assetsDir",
             "${assets_root}", "--assetIndex", "${assets_index_name}",
             "--uuid", "${auth_uuid}", "--userType", "${user_type}",
             "--versionType", "${version_type}", NULL);
  resolved->logging_argument = NULL;

  argv = aml_launch_build_arguments (resolved, "/usr/bin/java",
                                     "/home/u/games/one", NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (argv);

  /* An old document says nothing about where the natives are and which
     jars are on the classpath, so AML adds both. */
  g_assert_cmpstr (at (argv, 0), ==, "/usr/bin/java");
  g_assert_cmpstr (at (argv, 1), ==,
                   "-Djava.library.path=/home/u/.local/share/AML/natives");
  g_assert_cmpstr (at (argv, 2), ==, "-cp");
  g_assert_cmpstr (at (argv, 3), ==, resolved->classpath);

  /* The main class comes after the JVM, with the game arguments behind
     it. */
  g_assert_cmpstr (at (argv, 4), ==, "net.minecraft.client.main.Main");

  for (guint i = 0; i < argv->len; i++)
    {
      if (g_strcmp0 (at (argv, i), "-cp") == 0)
        classpath = i;

      if (g_strcmp0 (at (argv, i), "net.minecraft.client.main.Main") == 0)
        main_class = i;
    }

  g_assert_cmpuint (main_class, >, classpath);
  g_assert_cmpstr (at (argv, main_class + 1), ==, "--username");
  g_assert_cmpstr (at (argv, main_class + 2), ==, AML_LAUNCH_OFFLINE_NAME);
  g_assert_cmpstr (at (argv, main_class + 4), ==, "1.8.9");
  g_assert_cmpstr (at (argv, main_class + 6), ==, "/home/u/games/one");
  g_assert_cmpstr (at (argv, main_class + 14), ==, "legacy");
}

static void
test_build_arguments_failures (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.20.1");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;

  resolved->jvm_arguments = strings (NULL);
  resolved->game_arguments = strings ("--demo", NULL);
  g_clear_pointer (&resolved->main_class, g_free);

  argv = aml_launch_build_arguments (resolved, "/usr/bin/java",
                                     "/home/u/games/one", NULL, &error);

  g_assert_null (argv);
  g_assert_error (error, AML_LAUNCH_ERROR, AML_LAUNCH_ERROR_NO_MAIN_CLASS);
  g_assert_nonnull (strstr (error->message, "1.20.1"));
  g_clear_error (&error);

  /* JVM arguments that are not a command line fail the launch instead of
     being split by guesswork. */
  resolved->main_class = g_strdup ("net.minecraft.client.main.Main");
  argv = aml_launch_build_arguments (resolved, "/usr/bin/java",
                                     "/home/u/games/one", "-Xmx2G \"unclosed",
                                     &error);

  g_assert_null (argv);
  g_assert_error (error, AML_LAUNCH_ERROR, AML_LAUNCH_ERROR_ARGUMENTS);
}

/* ---------------------------------------------------------- *
 * Redaction
 * ---------------------------------------------------------- */

static void
test_redact (void)
{
  g_autoptr (GPtrArray) argv = strings (
    "/usr/bin/java", "-Xmx2G", "--accessToken", "s3cr3t-token", "--username",
    "Player", "--password=hunter2", NULL);
  g_autofree char *shown = aml_launch_redact (argv);
  g_autoptr (GPtrArray) plain = strings ("/usr/bin/java", "-cp",
                                         "/games/one", "--demo", NULL);
  g_autoptr (GPtrArray) nothing = strings (NULL);
  g_autoptr (GPtrArray) blank = strings ("java", "--clientId", "", NULL);
  g_autofree char *shown_plain = aml_launch_redact (plain);
  g_autofree char *empty = aml_launch_redact (nothing);
  g_autofree char *shown_blank = aml_launch_redact (blank);

  /* The token is gone, both as the value of the flag and as one
     argument. The stars are quoted, because a shell would read them as
     a glob. */
  g_assert_null (strstr (shown, "s3cr3t-token"));
  g_assert_null (strstr (shown, "hunter2"));
  g_assert_nonnull (strstr (shown, "--accessToken '***'"));
  g_assert_nonnull (strstr (shown, "--password='***'"));

  /* Nothing else is touched, so the log still says what ran, and an
     argument with a space in it is quoted the way a shell needs. */
  g_assert_cmpstr (shown_plain, ==, "/usr/bin/java -cp /games/one --demo");
  g_assert_cmpstr (empty, ==, "");

  /* An empty argument is quoted rather than dropped: a command line
     without it would name a different command. */
  g_assert_cmpstr (shown_blank, ==, "java --clientId ''");
}

int
main (void)
{
  g_print ("# Launch argument tests\n");

  test_offline_uuid ();
  g_print ("ok - the offline UUID is the one java derives\n");

  test_substitute_known_and_unknown ();
  g_print ("ok - known placeholders are replaced, unknown ones are kept\n");

  test_build_arguments_modern ();
  g_print ("ok - a modern document builds the command line it documents\n");

  test_build_arguments_legacy ();
  g_print ("ok - a legacy document gets the library path and classpath\n");

  test_build_arguments_failures ();
  g_print ("ok - a document without a main class or bad jvm args fails\n");

  test_redact ();
  g_print ("ok - the token is redacted everywhere the command line shows\n");

  return EXIT_SUCCESS;
}