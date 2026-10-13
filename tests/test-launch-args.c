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
  resolved->version_type = g_strdup ("release");
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
 * Who a launch runs as
 * ---------------------------------------------------------- */

/* The session of an account is what the game is told: the name and the
 * UUID of the account itself, its token, and the user type that says
 * which kind of account it is. An offline account has no token of its
 * own and is given the one the game cannot authenticate with, which is
 * also what says the session is not a real one. */
static void
test_session_of_an_account (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlAccount) offline = aml_account_new_offline ("Flia", &error);
  AmlLaunchSession session;

  g_assert_no_error (error);

  session = aml_launch_session_for (offline);
  g_assert_cmpstr (session.name, ==, "Flia");
  g_assert_cmpstr (session.uuid, ==, "41b05633-42e0-3a6f-b64b-6b64448dccdd");
  g_assert_cmpstr (session.access_token, ==, AML_LAUNCH_OFFLINE_TOKEN);
  g_assert_cmpstr (session.user_type, ==, AML_LAUNCH_OFFLINE_USER_TYPE);

  /* A Microsoft account is its own name, its own UUID and its own token,
     and the user type that says it is a real one. */
  {
    g_autoptr (AmlAccount) microsoft = g_new0 (AmlAccount, 1);
    g_autoptr (AmlAccount) without_token = NULL;

    microsoft->type = AML_ACCOUNT_MICROSOFT;
    microsoft->id = g_strdup ("069a79f444e94726a5befca90e38aaf5");
    microsoft->name = g_strdup ("Notch");
    microsoft->uuid = g_strdup ("069a79f4-44e9-4726-a5be-fca90e38aaf5");
    microsoft->access_token = g_strdup ("the-token-of-the-session");

    session = aml_launch_session_for (microsoft);
    g_assert_cmpstr (session.name, ==, "Notch");
    g_assert_cmpstr (session.uuid, ==, "069a79f4-44e9-4726-a5be-fca90e38aaf5");
    g_assert_cmpstr (session.access_token, ==, "the-token-of-the-session");
    g_assert_cmpstr (session.user_type, ==, AML_LAUNCH_MICROSOFT_USER_TYPE);

    /* An account whose token is gone cannot be given one that is not
       there: the offline token is what the game is told rather than a
       null it would not survive. */
    without_token = aml_account_copy (microsoft);
    g_clear_pointer (&without_token->access_token, g_free);

    session = aml_launch_session_for (without_token);
    g_assert_cmpstr (session.access_token, ==, AML_LAUNCH_OFFLINE_TOKEN);
    g_assert_cmpstr (session.user_type, ==, AML_LAUNCH_MICROSOFT_USER_TYPE);
  }
}

/* The command line of a launch carries the account, token and all: what
   the game is told is what the launcher does not make up. */
static void
test_arguments_carry_the_account (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.20.1");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (AmlAccount) account = g_new0 (AmlAccount, 1);
  AmlLaunchSession session;

  account->type = AML_ACCOUNT_MICROSOFT;
  account->id = g_strdup ("069a79f444e94726a5befca90e38aaf5");
  account->name = g_strdup ("Notch");
  account->uuid = g_strdup ("069a79f4-44e9-4726-a5be-fca90e38aaf5");
  account->access_token = g_strdup ("the-token-of-the-session");

  session = aml_launch_session_for (account);

  resolved->jvm_arguments = strings (NULL);
  resolved->game_arguments =
    strings ("--username", "${auth_player_name}", "--uuid", "${auth_uuid}",
             "--accessToken", "${auth_access_token}", "--userType",
             "${user_type}", NULL);
  resolved->logging_argument = NULL;

  argv = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
                                     "/home/u/games/one", NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (argv);

  /* This document says nothing about the natives or the classpath, so
     AML adds both before the main class: java, the library path, "-cp",
     the classpath, the main class and then the six words of the game
     arguments. */
  g_assert_cmpstr (at (argv, 4), ==, "net.minecraft.client.main.Main");
  g_assert_cmpstr (at (argv, 5), ==, "--username");
  g_assert_cmpstr (at (argv, 6), ==, "Notch");
  g_assert_cmpstr (at (argv, 7), ==, "--uuid");
  g_assert_cmpstr (at (argv, 8), ==, "069a79f4-44e9-4726-a5be-fca90e38aaf5");
  g_assert_cmpstr (at (argv, 9), ==, "--accessToken");
  g_assert_cmpstr (at (argv, 10), ==, "the-token-of-the-session");
  g_assert_cmpstr (at (argv, 11), ==, "--userType");
  g_assert_cmpstr (at (argv, 12), ==, "msa");

  /* The token is in the command line and never in what is shown of it: the
     only form of a command line AML prints is the redacted one. */
  {
    g_autofree char *shown = aml_launch_redact (argv);

    g_assert_null (strstr (shown, "the-token-of-the-session"));
    g_assert_nonnull (strstr (shown, "--accessToken '***'"));
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
  g_autoptr (AmlAccount) account = aml_account_new_offline ("Flia", NULL);
  AmlLaunchSession session;
  guint classpaths = 0;
  guint library_paths = 0;

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

  session = aml_launch_session_for (account);

  argv = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
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
  g_assert_cmpstr (at (argv, 9), ==, "Flia");
  g_assert_cmpstr (at (argv, 11), ==, "1.20.1");
  g_assert_cmpstr (at (argv, 13), ==, "/home/u/games/one");
  g_assert_cmpstr (at (argv, 15), ==, "/home/u/.local/share/AML/assets");
  g_assert_cmpstr (at (argv, 17), ==, "12");
  g_assert_cmpstr (at (argv, 19), ==, account->uuid);
  g_assert_cmpstr (at (argv, 21), ==, AML_LAUNCH_OFFLINE_USER_TYPE);
  g_assert_cmpstr (at (argv, 23), ==, "release");
}

/* ---------------------------------------------------------- *
 * A document and no instance arguments
 * ---------------------------------------------------------- */

/* An instance nobody wrote arguments for stores an empty string, which
 * is the default of a new instance. Nothing of it reaches the command
 * line: the line a modern document builds is the same one it builds with
 * no instance arguments at all. */
static void
test_build_arguments_without_jvm_args (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.20.1");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) with_empty = NULL;
  g_autoptr (GPtrArray) with_none = NULL;
  g_autoptr (AmlAccount) account = aml_account_new_offline ("Flia", NULL);
  AmlLaunchSession session;

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

  session = aml_launch_session_for (account);

  with_empty = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
                                           "/home/u/games/one", "", &error);
  g_assert_no_error (error);
  g_assert_nonnull (with_empty);

  /* A missing value must behave the same, in case an instance written by
     hand left the field out entirely. */
  with_none = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
                                         "/home/u/games/one", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (with_none);

  g_assert_cmpuint (with_empty->len, ==, with_none->len);
  g_assert_cmpuint (with_empty->len, ==, 6 + 16);

  for (guint i = 0; i < with_empty->len; i++)
    g_assert_cmpstr (at (with_empty, i), ==, at (with_none, i));

  /* Nothing that looks like a heap size was invented for it. */
  for (guint i = 0; i < with_empty->len; i++)
    g_assert_null (strstr (at (with_empty, i), "-Xmx"));
}

static void
test_version_type_fallback (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.20.1");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (AmlAccount) account = aml_account_new_offline ("Flia", NULL);
  AmlLaunchSession session = aml_launch_session_for (account);

  g_clear_pointer (&resolved->version_type, g_free);
  resolved->jvm_arguments = strings (NULL);
  resolved->game_arguments = strings ("${version_type}", NULL);

  argv = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
                                     "/home/u/games/one", NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (argv);
  g_assert_cmpstr (at (argv, argv->len - 1), ==, "1.20.1");
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
  g_autoptr (AmlAccount) account = aml_account_new_offline ("Flia", NULL);
  AmlLaunchSession session;
  guint classpath = 0;
  guint main_class = 0;

  session = aml_launch_session_for (account);

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

  argv = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
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
  g_assert_cmpstr (at (argv, main_class + 2), ==, "Flia");
  g_assert_cmpstr (at (argv, main_class + 4), ==, "1.8.9");
  g_assert_cmpstr (at (argv, main_class + 6), ==, "/home/u/games/one");
  g_assert_cmpstr (at (argv, main_class + 14), ==, "legacy");
}

static void
test_build_arguments_legacy_session (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.6.4");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (AmlAccount) account = g_new0 (AmlAccount, 1);
  AmlLaunchSession session;
  guint main_class = 0;

  account->type = AML_ACCOUNT_MICROSOFT;
  account->name = g_strdup ("Notch");
  account->uuid = g_strdup ("069a79f4-44e9-4726-a5be-fca90e38aaf5");
  account->access_token = g_strdup ("the-access-token");
  session = aml_launch_session_for (account);

  resolved->jvm_arguments = strings (NULL);
  resolved->game_arguments = strings ("--username", "${auth_player_name}",
                                      "--session", "${auth_session}", NULL);

  argv = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
                                     "/home/u/games/one", NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (argv);

  for (guint i = 0; i < argv->len; i++)
    if (g_strcmp0 (at (argv, i), "net.minecraft.client.main.Main") == 0)
      main_class = i;

  g_assert_cmpstr (at (argv, main_class + 4), ==,
                   "token:the-access-token:069a79f4-44e9-4726-a5be-fca90e38aaf5");

  {
    g_autofree char *shown = aml_launch_redact (argv);
    g_assert_null (strstr (shown, "the-access-token"));
    g_assert_nonnull (strstr (shown, "--session '***'"));
  }
}

static void
test_build_arguments_failures (void)
{
  g_autoptr (AmlResolvedVersion) resolved = resolved_new ("1.20.1");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (AmlAccount) account = aml_account_new_offline ("Flia", NULL);
  AmlLaunchSession session;

  session = aml_launch_session_for (account);
  resolved->jvm_arguments = strings (NULL);
  resolved->game_arguments = strings ("--demo", NULL);
  g_clear_pointer (&resolved->main_class, g_free);

  argv = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
                                     "/home/u/games/one", NULL, &error);

  g_assert_null (argv);
  g_assert_error (error, AML_LAUNCH_ERROR, AML_LAUNCH_ERROR_NO_MAIN_CLASS);
  g_assert_nonnull (strstr (error->message, "1.20.1"));
  g_clear_error (&error);

  /* JVM arguments that are not a command line fail the launch instead of
     being split by guesswork. */
  resolved->main_class = g_strdup ("net.minecraft.client.main.Main");
  argv = aml_launch_build_arguments (resolved, &session, "/usr/bin/java",
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
    "Player", "--password=hunter2", "--session=legacy-secret",
    "--token", "token-secret", NULL);
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
  g_assert_null (strstr (shown, "legacy-secret"));
  g_assert_null (strstr (shown, "token-secret"));
  g_assert_nonnull (strstr (shown, "--accessToken '***'"));
  g_assert_nonnull (strstr (shown, "--password='***'"));
  g_assert_nonnull (strstr (shown, "--session='***'"));
  g_assert_nonnull (strstr (shown, "--token '***'"));

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

  test_session_of_an_account ();
  g_print ("ok - a launch runs as the account it is given\n");

  test_substitute_known_and_unknown ();
  g_print ("ok - known placeholders are replaced, unknown ones are kept\n");

  test_build_arguments_modern ();
  g_print ("ok - a modern document builds the command line it documents\n");

  test_build_arguments_without_jvm_args ();
  g_print ("ok - no jvm arguments adds nothing to the command line\n");

  test_version_type_fallback ();
  g_print ("ok - a missing version type falls back to the version id\n");

  test_build_arguments_legacy ();
  g_print ("ok - a legacy document gets the library path and classpath\n");

  test_build_arguments_legacy_session ();
  g_print ("ok - legacy sessions carry the token and stay redacted\n");

  test_build_arguments_failures ();
  g_print ("ok - a document without a main class or bad jvm args fails\n");

  test_arguments_carry_the_account ();
  g_print ("ok - the command line carries the account and no token in print\n");

  test_redact ();
  g_print ("ok - the token is redacted everywhere the command line shows\n");

  return EXIT_SUCCESS;
}
