/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-launcher.h"
#include "nuvasha-i18n.h"

#include <string.h>

/* ---------------------------------------------------------- *
 * The session
 * ---------------------------------------------------------- */

/* Build the player session values used by the version placeholders. */
NuvashaLaunchSession
nuvasha_launch_session_for (const NuvashaAccount *account)
{
  NuvashaLaunchSession session = { NULL, NULL, NULL, NULL };

  g_return_val_if_fail (account != NULL, session);

  session.name = account->name != NULL ? account->name : "";
  session.uuid = account->uuid != NULL ? account->uuid : "";
  session.user_type = account->type == nuvasha_ACCOUNT_MICROSOFT
                        ? nuvasha_LAUNCH_MICROSOFT_USER_TYPE
                        : nuvasha_LAUNCH_OFFLINE_USER_TYPE;
  session.access_token = account->type == nuvasha_ACCOUNT_MICROSOFT &&
                                 account->access_token != NULL &&
                                 *account->access_token != '\0'
                           ? account->access_token
                           : nuvasha_LAUNCH_OFFLINE_TOKEN;

  return session;
}

/* ---------------------------------------------------------- *
 * The placeholders
 * ---------------------------------------------------------- */

/* Every ${name} the table knows is replaced by its value; a name the
 * table does not hold is left exactly as the document wrote it, because
 * a launcher that does not know a placeholder must not silently drop
 * it. */
char *
nuvasha_launch_substitute (const char *value, GHashTable *placeholders)
{
  GString *out;
  const char *p;

  if (value == NULL)
    return NULL;

  out = g_string_new (NULL);

  for (p = value; *p != '\0';)
    {
      const char *end = NULL;
      const char *replacement = NULL;

      if (p[0] == '$' && p[1] == '{')
        {
          g_autofree char *name = NULL;

          end = strchr (p + 2, '}');

          if (end != NULL)
            {
              name = g_strndup (p + 2, (gsize) (end - p - 2));
              replacement = g_hash_table_lookup (placeholders, name);
            }
        }

      if (replacement == NULL)
        {
          g_string_append_c (out, *p);
          p++;
          continue;
        }

      g_string_append (out, replacement);
      p = end + 1;
    }

  return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------- *
 * The command line
 * ---------------------------------------------------------- */

static void
put_placeholder (GHashTable *table, const char *name, const char *value)
{
  g_hash_table_insert (table, g_strdup (name),
                       g_strdup (value != NULL ? value : ""));
}

static gboolean
argv_has (GPtrArray *argv, const char *option, const char *prefix)
{
  for (guint i = 0; i < argv->len; i++)
    {
      const char *argument = g_ptr_array_index (argv, i);

      if (option != NULL && g_strcmp0 (argument, option) == 0)
        return TRUE;

      if (prefix != NULL && g_str_has_prefix (argument, prefix))
        return TRUE;
    }

  return FALSE;
}

/* Appends the words of one command line the way a shell would split
 * them. Fails rather than guesses when the line does not parse. */
static gboolean
add_shell_words (GPtrArray *argv, const char *line, GError **error)
{
  g_auto (GStrv) words = NULL;
  int count = 0;

  if (line == NULL || *line == '\0')
    return TRUE;

  if (!g_shell_parse_argv (line, &count, &words, NULL))
    {
      g_set_error (error, nuvasha_LAUNCH_ERROR, nuvasha_LAUNCH_ERROR_ARGUMENTS,
                   "the JVM arguments of the instance are not a command "
                   "line: \"%s\"", line);
      return FALSE;
    }

  for (int i = 0; i < count; i++)
    g_ptr_array_add (argv, g_strdup (words[i]));

  return TRUE;
}

GPtrArray *
nuvasha_launch_build_arguments (const NuvashaResolvedVersion *resolved,
                            const NuvashaLaunchSession   *session,
                            const char             *java_executable,
                            const char             *game_directory,
                            const char             *jvm_args,
                            GError                **error)
{
  g_autoptr (GHashTable) placeholders = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autofree char *legacy_session = NULL;
  const char *auth_session = nuvasha_LAUNCH_OFFLINE_TOKEN;

  g_return_val_if_fail (resolved != NULL, NULL);
  g_return_val_if_fail (session != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (resolved->main_class == NULL || resolved->main_class[0] == '\0')
    {
      g_set_error (error, nuvasha_LAUNCH_ERROR, nuvasha_LAUNCH_ERROR_NO_MAIN_CLASS,
                   "the version document of %s names no main class",
                   resolved->version_id != NULL ? resolved->version_id
                                                : "this version");
      return NULL;
    }

  placeholders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* Minecraft versions through 1.6 use one legacy session argument rather
     than the separate UUID and access-token arguments used by newer
     versions. The old launcher format is token:<access-token>:<uuid>. */
  if (g_strcmp0 (session->user_type, nuvasha_LAUNCH_MICROSOFT_USER_TYPE) == 0 &&
      session->access_token != NULL && *session->access_token != '\0' &&
      g_strcmp0 (session->access_token, nuvasha_LAUNCH_OFFLINE_TOKEN) != 0)
    {
      legacy_session = g_strdup_printf ("token:%s:%s", session->access_token,
                                        session->uuid != NULL ? session->uuid
                                                               : "");
      auth_session = legacy_session;
    }

  /* Who the player is: the account, not a name nuvasha made up. */
  put_placeholder (placeholders, "auth_player_name", session->name);
  put_placeholder (placeholders, "auth_uuid", session->uuid);
  put_placeholder (placeholders, "auth_access_token", session->access_token);
  put_placeholder (placeholders, "auth_session", auth_session);
  put_placeholder (placeholders, "auth_xuid", "");
  put_placeholder (placeholders, "clientid", "");
  put_placeholder (placeholders, "user_type", session->user_type);
  put_placeholder (placeholders, "user_properties", "{}");
  put_placeholder (placeholders, "launcher_name", nuvasha_LAUNCHER_NAME);
  put_placeholder (placeholders, "launcher_version", nuvasha_LAUNCHER_VERSION);

  /* What the machine knows. The paths the resolve module already
     substituted are repeated here so this function is the whole answer
     on its own. */
  put_placeholder (placeholders, "game_directory", game_directory);
  put_placeholder (placeholders, "version_name", resolved->version_id);
  put_placeholder (placeholders, "version_type",
                   resolved->version_type != NULL &&
                   resolved->version_type[0] != '\0'
                     ? resolved->version_type : resolved->version_id);
  put_placeholder (placeholders, "natives_directory",
                   resolved->natives_directory);
  put_placeholder (placeholders, "assets_root", resolved->assets_directory);
  /* Pre-1.7 version documents call the same directory game_assets. */
  put_placeholder (placeholders, "game_assets", resolved->assets_directory);
  put_placeholder (placeholders, "assets_index_name",
                   resolved->assets_index_name);
  put_placeholder (placeholders, "library_directory",
                   resolved->library_directory);
  put_placeholder (placeholders, "classpath", resolved->classpath);
  put_placeholder (placeholders, "classpath_separator",
                   G_SEARCHPATH_SEPARATOR_S);
  put_placeholder (placeholders, "resolution_width",
                   G_STRINGIFY (nuvasha_LAUNCH_WIDTH));
  put_placeholder (placeholders, "resolution_height",
                   G_STRINGIFY (nuvasha_LAUNCH_HEIGHT));

  argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (java_executable));

  /* What the version document says, with the placeholders replaced. */
  if (resolved->jvm_arguments != NULL)
    {
      for (guint i = 0; i < resolved->jvm_arguments->len; i++)
        g_ptr_array_add (
          argv,
          nuvasha_launch_substitute (g_ptr_array_index (resolved->jvm_arguments, i),
                                 placeholders));
    }

  /* Then what the instance adds, which is one line the user wrote. */
  if (!add_shell_words (argv, jvm_args, error))
    return NULL;

  /* LWJGL 2 tries to execute xrandr without checking whether it exists. This
   * disables that path and lets LWJGL use its XF86VidMode fallback when
   * xrandr is unavailable. LWJGL 3 does not
   * read this property, so it is safe to add it only to old classpaths. */
  if (resolved->classpath != NULL &&
      (strstr (resolved->classpath, "/org/lwjgl/lwjgl/lwjgl/") != NULL ||
       strstr (resolved->classpath, "/org/lwjgl/lwjgl/lwjgl-platform/") != NULL))
    {
      g_autofree char *xrandr = g_find_program_in_path ("xrandr");

      if (xrandr == NULL)
        g_ptr_array_add (argv, g_strdup ("-DLWJGL_DISABLE_XRANDR=true"));
    }

  /* A document of 1.13 and newer already carries the library path and
     the classpath; an older one carries neither, so both go here when
     they are missing instead of twice when they are not. */
  if (!argv_has (argv, NULL, "-Djava.library.path="))
    g_ptr_array_add (argv, g_strdup_printf (
                           "-Djava.library.path=%s",
                           resolved->natives_directory != NULL
                             ? resolved->natives_directory
                             : ""));

  if (!argv_has (argv, "-cp", "-classpath"))
    {
      g_ptr_array_add (argv, g_strdup ("-cp"));
      g_ptr_array_add (argv, g_strdup (resolved->classpath));
    }

  if (resolved->logging_argument != NULL)
    g_ptr_array_add (argv,
                     nuvasha_launch_substitute (resolved->logging_argument,
                                            placeholders));

  g_ptr_array_add (argv, g_strdup (resolved->main_class));

  if (resolved->game_arguments != NULL)
    {
      for (guint i = 0; i < resolved->game_arguments->len; i++)
        g_ptr_array_add (
          argv,
          nuvasha_launch_substitute (
            g_ptr_array_index (resolved->game_arguments, i), placeholders));
    }

  return g_steal_pointer (&argv);
}

/* g_shell_quote() quotes every word, which is what a terminal needs and
   what a log does not. An argument made only of characters a shell never
   treats specially is written the way it is; an empty one is quoted, or
   the command line would lose the argument altogether. */
static void
append_argument (GString *out, const char *argument)
{
  gboolean plain = *argument != '\0';

  for (const char *p = argument; *p != '\0'; p++)
    {
      if (!g_ascii_isalnum (*p) && strchr ("_-./:=+,@%^", *p) == NULL)
        {
          plain = FALSE;
          break;
        }
    }

  if (plain)
    {
      g_string_append (out, argument);
      return;
    }

  {
    g_autofree char *quoted = g_shell_quote (argument);

    g_string_append (out, quoted);
  }
}

char *
nuvasha_launch_redact (GPtrArray *argv)
{
  GString *out;
  gboolean hide_next = FALSE;

  g_return_val_if_fail (argv != NULL, g_strdup (""));

  out = g_string_new (NULL);

  for (guint i = 0; i < argv->len; i++)
    {
      const char *argument = g_ptr_array_index (argv, i);
      const char *equals = strchr (argument, '=');

      if (hide_next)
        {
          g_string_append_c (out, ' ');
          append_argument (out, "***");
          hide_next = FALSE;
          continue;
        }

      if (i > 0)
        g_string_append_c (out, ' ');

      /* Hide values supplied to flags that carry credentials. */
      if (g_strcmp0 (argument, "--accessToken") == 0 ||
          g_strcmp0 (argument, "--session") == 0 ||
          g_strcmp0 (argument, "--password") == 0 ||
          g_strcmp0 (argument, "--token") == 0)
        {
          g_string_append (out, argument);
          hide_next = TRUE;
          continue;
        }

      if ((g_str_has_prefix (argument, "--accessToken=") ||
           g_str_has_prefix (argument, "--session=") ||
           g_str_has_prefix (argument, "--password=") ||
           g_str_has_prefix (argument, "--token=")) &&
          equals != NULL)
        {
          g_string_append_len (out, argument, (gssize) (equals - argument));
          g_string_append_c (out, '=');
          append_argument (out, "***");
          continue;
        }

      append_argument (out, argument);
    }

  return g_string_free (out, FALSE);
}
