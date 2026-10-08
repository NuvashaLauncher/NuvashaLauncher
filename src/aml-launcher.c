/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlLauncher: putting one instance on the air and following it.
 *
 * The pure part comes first: the UUID of an offline player, the
 * substitution of the placeholders a version document writes, the whole
 * command line of a launch and the printable form of that command line
 * with the token taken out of it. Those are plain functions over data
 * that is already in memory, which is what the tests use.
 *
 * The pipeline on top of that is asynchronous from end to end: the
 * manifest, the resolve module putting every jar, asset and native on
 * disk, the Java detection, and GSubprocess. Nothing here waits for a
 * socket, a disk or a process in the caller's main context.
 *
 * The object owns the subprocess, reads its two pipes line by line with
 * a timestamp on every line and holds the state. A state only ever comes
 * from something that really happened: progress from the resolve, a
 * process that spawned, a process that ended.
 *
 * The game is not AML: it does not inherit the GDK_BACKEND=wayland AML
 * sets for itself, so LWJGL opens its window through XWayland as it does
 * for every other launcher on this desktop. Nothing else about the
 * environment is touched. */

#include "aml-launcher.h"
#include "aml-i18n.h"

#include "aml-auth-microsoft.h"
#include "aml-java.h"
#include "aml-loaders.h"
#include "aml-minecraft-resolve.h"
#include "aml-minecraft-versions.h"

#include <signal.h>
#include <string.h>

/* The three kinds of line the log holds. */
#define TAG_STATE "state"
#define TAG_STDOUT "stdout"
#define TAG_STDERR "stderr"

GQuark
aml_launch_error_quark (void)
{
  return g_quark_from_static_string ("aml-launch-error");
}

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* One table for the enum type of the signal parameter and the names the
 * enum members are spelled with. */
static const GEnumValue launch_state_values[] = {
  { AML_LAUNCH_PREPARING,      "AML_LAUNCH_PREPARING",      "Preparing"     },
  { AML_LAUNCH_DOWNLOADING,    "AML_LAUNCH_DOWNLOADING",    "Downloading"   },
  { AML_LAUNCH_LAUNCHING,      "AML_LAUNCH_LAUNCHING",      "Launching"     },
  { AML_LAUNCH_RUNNING,        "AML_LAUNCH_RUNNING",        "Running"       },
  { AML_LAUNCH_STOPPING,       "AML_LAUNCH_STOPPING",       "Stopping"      },
  { AML_LAUNCH_STOPPED,        "AML_LAUNCH_STOPPED",        "Stopped"       },
  { AML_LAUNCH_CRASHED,        "AML_LAUNCH_CRASHED",        "Crashed"       },
  { AML_LAUNCH_FAILED,         "AML_LAUNCH_FAILED",         "Failed"        },
  { AML_LAUNCH_NEEDS_SIGN_IN,  "AML_LAUNCH_NEEDS_SIGN_IN",  "Needs sign-in" },
  { 0, NULL, NULL }
};

GType
aml_launch_state_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType state_type = g_enum_register_static ("AmlLaunchState",
                                                 launch_state_values);
      g_once_init_leave (&type_id, state_type);
    }

  return type_id;
}

const char *
aml_launch_state_to_string (AmlLaunchState state)
{
  for (guint i = 0; launch_state_values[i].value_name != NULL; i++)
    {
      if ((AmlLaunchState) launch_state_values[i].value == state)
        return launch_state_values[i].value_nick;
    }

  return "Unknown";
}

gboolean
aml_launch_state_is_busy (AmlLaunchState state)
{
  return state == AML_LAUNCH_PREPARING || state == AML_LAUNCH_DOWNLOADING ||
         state == AML_LAUNCH_LAUNCHING || state == AML_LAUNCH_RUNNING ||
         state == AML_LAUNCH_STOPPING;
}

/* ---------------------------------------------------------- *
 * The session
 * ---------------------------------------------------------- */

/* What the game is told about the player, in the four words the version
 * document writes its placeholders for.
 *
 * An offline account has no token of its own and the game is told the one
 * it cannot use, so that the argument is there with or without a real
 * account behind it: that argument is also what tells the game the
 * session is not a real one. */
AmlLaunchSession
aml_launch_session_for (const AmlAccount *account)
{
  AmlLaunchSession session = { NULL, NULL, NULL, NULL };

  g_return_val_if_fail (account != NULL, session);

  session.name = account->name != NULL ? account->name : "";
  session.uuid = account->uuid != NULL ? account->uuid : "";
  session.user_type = account->type == AML_ACCOUNT_MICROSOFT
                        ? AML_LAUNCH_MICROSOFT_USER_TYPE
                        : AML_LAUNCH_OFFLINE_USER_TYPE;
  session.access_token = account->type == AML_ACCOUNT_MICROSOFT &&
                                 account->access_token != NULL &&
                                 *account->access_token != '\0'
                           ? account->access_token
                           : AML_LAUNCH_OFFLINE_TOKEN;

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
aml_launch_substitute (const char *value, GHashTable *placeholders)
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
      g_set_error (error, AML_LAUNCH_ERROR, AML_LAUNCH_ERROR_ARGUMENTS,
                   "the JVM arguments of the instance are not a command "
                   "line: \"%s\"", line);
      return FALSE;
    }

  for (int i = 0; i < count; i++)
    g_ptr_array_add (argv, g_strdup (words[i]));

  return TRUE;
}

GPtrArray *
aml_launch_build_arguments (const AmlResolvedVersion *resolved,
                            const AmlLaunchSession   *session,
                            const char             *java_executable,
                            const char             *game_directory,
                            const char             *jvm_args,
                            GError                **error)
{
  g_autoptr (GHashTable) placeholders = NULL;
  g_autoptr (GPtrArray) argv = NULL;

  g_return_val_if_fail (resolved != NULL, NULL);
  g_return_val_if_fail (session != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (resolved->main_class == NULL || resolved->main_class[0] == '\0')
    {
      g_set_error (error, AML_LAUNCH_ERROR, AML_LAUNCH_ERROR_NO_MAIN_CLASS,
                   "the version document of %s names no main class",
                   resolved->version_id != NULL ? resolved->version_id
                                                : "this version");
      return NULL;
    }

  placeholders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* Who the player is: the account, not a name AML made up. */
  put_placeholder (placeholders, "auth_player_name", session->name);
  put_placeholder (placeholders, "auth_uuid", session->uuid);
  put_placeholder (placeholders, "auth_access_token", session->access_token);
  put_placeholder (placeholders, "auth_session", "");
  put_placeholder (placeholders, "auth_xuid", "");
  put_placeholder (placeholders, "clientid", "");
  put_placeholder (placeholders, "user_type", session->user_type);
  put_placeholder (placeholders, "user_properties", "{}");
  put_placeholder (placeholders, "launcher_name", AML_LAUNCHER_NAME);
  put_placeholder (placeholders, "launcher_version", AML_LAUNCHER_VERSION);

  /* What the machine knows. The paths the resolve module already
     substituted are repeated here so this function is the whole answer
     on its own. */
  put_placeholder (placeholders, "game_directory", game_directory);
  put_placeholder (placeholders, "version_name", resolved->version_id);
  put_placeholder (placeholders, "version_type", resolved->version_id);
  put_placeholder (placeholders, "natives_directory",
                   resolved->natives_directory);
  put_placeholder (placeholders, "assets_root", resolved->assets_directory);
  put_placeholder (placeholders, "assets_index_name",
                   resolved->assets_index_name);
  put_placeholder (placeholders, "library_directory",
                   resolved->library_directory);
  put_placeholder (placeholders, "classpath", resolved->classpath);
  put_placeholder (placeholders, "classpath_separator",
                   G_SEARCHPATH_SEPARATOR_S);
  put_placeholder (placeholders, "resolution_width",
                   G_STRINGIFY (AML_LAUNCH_WIDTH));
  put_placeholder (placeholders, "resolution_height",
                   G_STRINGIFY (AML_LAUNCH_HEIGHT));

  argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (java_executable));

  /* What the version document says, with the placeholders replaced. */
  if (resolved->jvm_arguments != NULL)
    {
      for (guint i = 0; i < resolved->jvm_arguments->len; i++)
        g_ptr_array_add (
          argv,
          aml_launch_substitute (g_ptr_array_index (resolved->jvm_arguments, i),
                                 placeholders));
    }

  /* Then what the instance adds, which is one line the user wrote. */
  if (!add_shell_words (argv, jvm_args, error))
    return NULL;

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
                     aml_launch_substitute (resolved->logging_argument,
                                            placeholders));

  g_ptr_array_add (argv, g_strdup (resolved->main_class));

  if (resolved->game_arguments != NULL)
    {
      for (guint i = 0; i < resolved->game_arguments->len; i++)
        g_ptr_array_add (
          argv,
          aml_launch_substitute (
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
aml_launch_redact (GPtrArray *argv)
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

      /* The token of the session goes into the command line as the
         argument of --accessToken; nothing else a launcher passes holds
         a secret, but the two forms below are covered as well. */
      if (g_strcmp0 (argument, "--accessToken") == 0 ||
          g_strcmp0 (argument, "--password") == 0)
        {
          g_string_append (out, argument);
          hide_next = TRUE;
          continue;
        }

      if ((g_str_has_prefix (argument, "--accessToken=") ||
           g_str_has_prefix (argument, "--password=")) &&
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

/* ---------------------------------------------------------- *
 * The object
 * ---------------------------------------------------------- */

struct _AmlLauncher
{
  GObject parent_instance;

  AmlInstance *instance;
  AmlSettings *settings;
  char *version;
  char *game_directory;

  AmlLaunchState state;
  char *detail; /* what goes with the state, NULL when there is nothing */

  /* The log: the newest line last, trimmed to what the settings ask
     for. */
  GPtrArray *lines;

  /* What a launch needs while it is on its way. */
  GCancellable *cancellable;
  AmlResolvedVersion *resolved;
  GSubprocess *process;

  /* The accounts a launch plays as. The active one of them is read when
     the command line is built, so a token that was refreshed a moment
     earlier is the one the game is run with. */
  AmlAccounts *accounts;

  /* The Java this launch chose. A loader that has to be installed first
   * needs one before the game is resolved, so the choice is made once and
   * kept. */
  char *java_executable;
  int java_major_version;

  /* How the process ended, filled in when it is gone. */
  gboolean exited;
  gboolean exited_normally;
  gboolean terminated_by_signal;
  int exit_status;
  int term_signal;
  gint64 started_at; /* unix seconds, for the crash reports */
  guint readers;     /* output readers that have not reached the end */
  guint stop_timeout;
  guint last_percent; /* how far the resolve got the last time it said */

  gboolean stop_requested;
};

G_DEFINE_FINAL_TYPE (AmlLauncher, aml_launcher, G_TYPE_OBJECT)

static guint aml_launcher_signals[AML_LAUNCHER_N_SIGNALS];

static void on_resolved (GObject *source, GAsyncResult *result,
                         gpointer user_data);

static void launcher_needs_sign_in (AmlLauncher *self, const char *reason);

/* The account a launch plays as, or NULL when there is none. A launcher
 * made without a store is a launcher with no account, which is what a
 * test that drives one step at a time is. */
static AmlAccount *
active_account (AmlLauncher *self)
{
  if (self->accounts == NULL)
    return NULL;

  return aml_accounts_get_active (self->accounts);
}

/* ---------------------------------------------------------- *
 * The log
 * ---------------------------------------------------------- */

guint
aml_launcher_get_line_limit (AmlLauncher *self)
{
  guint limit;

  g_return_val_if_fail (AML_IS_LAUNCHER (self), 0);

  /* Read for every line that comes in, so a limit the user changes
     while a game runs takes effect at once. */
  limit = self->settings != NULL
            ? aml_settings_get_log_line_limit (self->settings)
            : AML_SETTINGS_DEFAULT_LOG_LINE_LIMIT;

  return limit > 0 ? limit : AML_SETTINGS_DEFAULT_LOG_LINE_LIMIT;
}

/* One line of the log: the time it happened, where it came from and the
 * line itself. Everything a game prints goes through here, and so does
 * everything AML says about the launch. */
static void
log_line (AmlLauncher *self,
          const char  *tag,
          const char  *format,
          ...) G_GNUC_PRINTF (3, 4);

static void
log_line (AmlLauncher *self,
          const char  *tag,
          const char  *format,
          ...)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = NULL;
  g_autofree char *body = NULL;
  g_autofree char *line = NULL;
  va_list args;
  guint limit;

  va_start (args, format);
  body = g_strdup_vprintf (format, args);
  va_end (args);

  stamp = now != NULL ? g_date_time_format (now, "%H:%M:%S")
                      : g_strdup ("--:--:--");

  /* The time it happened, where it came from and the line itself. */
  line = g_strdup_printf ("%s [%s] %s", stamp, tag, body);

  limit = aml_launcher_get_line_limit (self);

  g_ptr_array_add (self->lines, g_strdup (line));

  /* The oldest line goes first: a long game must not grow the log for
     ever. */
  while (self->lines->len > limit)
    g_ptr_array_remove_index (self->lines, 0);

  g_signal_emit (self, aml_launcher_signals[AML_LAUNCHER_SIGNAL_OUTPUT], 0,
                 line, g_strcmp0 (tag, TAG_STDERR) == 0);
}

guint
aml_launcher_get_line_count (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), 0);

  return self->lines->len;
}

char *
aml_launcher_get_log_text (AmlLauncher *self)
{
  GString *text;

  g_return_val_if_fail (AML_IS_LAUNCHER (self), g_strdup (""));

  text = g_string_new (NULL);

  for (guint i = 0; i < self->lines->len; i++)
    {
      if (i > 0)
        g_string_append_c (text, '\n');

      g_string_append (text, g_ptr_array_index (self->lines, i));
    }

  return g_string_free (text, FALSE);
}

void
aml_launcher_clear_log (AmlLauncher *self)
{
  g_return_if_fail (AML_IS_LAUNCHER (self));

  g_ptr_array_set_size (self->lines, 0);
}

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* The state and the detail it goes with go out together, and only when
 * one of them actually changed: a resolve reports progress many times a
 * second and a view has no use for the same line twice. */
static void
set_state (AmlLauncher  *self,
           AmlLaunchState state,
           const char    *detail)
{
  gboolean changed;

  changed = self->state != state || g_strcmp0 (self->detail, detail) != 0;

  g_free (self->detail);
  self->detail = g_strdup (detail);
  self->state = state;

  if (changed)
    g_signal_emit (self, aml_launcher_signals[AML_LAUNCHER_SIGNAL_STATE], 0,
                   state, detail);
}

static void
launcher_failed (AmlLauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "%s", reason);
  set_state (self, AML_LAUNCH_FAILED, reason);
}

/* A launch that could not finish because the caller cancelled it, which
 * is what a stop does before the game starts, is not a failure. */
static void
launch_finished_with_error (AmlLauncher *self, GError *error)
{
  if (error != NULL &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      log_line (self, TAG_STATE,
                "the launch was cancelled before the game started");
      set_state (self, AML_LAUNCH_STOPPED, NULL);
      return;
    }

  launcher_failed (self, error != NULL ? error->message
                                       : "the launch failed");
}

/* ---------------------------------------------------------- *
 * Where the game ends
 * ---------------------------------------------------------- */

/* The desktop says no when it wants to. Nothing about that is worth a
   word in the log of a game: the folder is a convenience, and the button
   that asks for it is there for the next try. */
static void
log_folder_opened (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data G_GNUC_UNUSED)
{
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_default_for_uri_finish (result, &error))
    g_debug ("the log folder does not open: %s", error->message);
}

static void
open_log_folder (AmlLauncher *self)
{
  g_autofree char *directory = aml_launcher_get_log_directory (self);
  g_autofree char *uri = NULL;

  /* A game that never got far enough to write a log has no folder to
     open, which is not worth a word in the log. */
  if (!g_file_test (directory, G_FILE_TEST_IS_DIR))
    return;

  uri = g_filename_to_uri (directory, NULL, NULL);

  if (uri == NULL)
    return;

  /* GIO hands the folder to the desktop the same way a double click in
     a file manager does; nothing is run through a shell. */
  g_app_info_launch_default_for_uri_async (uri, NULL, NULL,
                                           log_folder_opened, NULL);
}

static void
game_crashed (AmlLauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "the game crashed: %s", reason);
  set_state (self, AML_LAUNCH_CRASHED, reason);

  if (self->settings != NULL &&
      aml_settings_get_open_logs_after_crash (self->settings))
    open_log_folder (self);
}

/* Nothing else can make a game that ended with zero a crash except a
 * report it wrote while it was dying, so that is what is asked for. A
 * report of an earlier run does not count: the launch time is what the
 * modification time is compared against. */
static void
crash_reports_listed (GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
  AmlLauncher *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileEnumerator) files = NULL;
  g_autoptr (GFileInfo) info = NULL;
  gboolean crashed = FALSE;

  files = g_file_enumerate_children_finish (G_FILE (source), result, &error);

  if (files != NULL)
    {
      while ((info = g_file_enumerator_next_file (files, NULL, NULL)) != NULL)
        {
          gboolean written_now = FALSE;

          if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
            written_now =
              g_file_info_get_attribute_uint64 (
                info, G_FILE_ATTRIBUTE_TIME_MODIFIED) >= (guint64) self->started_at;

          /* Every report is looked at, and what is left of the
             directory goes with the loop instead of piling up. */
          g_clear_object (&info);

          if (written_now)
            {
              crashed = TRUE;
              break;
            }
        }
    }

  if (crashed)
    game_crashed (self, "the game wrote a crash report");
  else
    set_state (self, AML_LAUNCH_STOPPED, NULL);

  g_object_unref (self);
}

static void
report_how_it_ended (AmlLauncher *self)
{
  g_autofree char *reason = NULL;

  if (self->terminated_by_signal)
    reason = g_strdup_printf (_("the game was stopped by signal %d"),
                             self->term_signal);
  else
    reason = g_strdup_printf (_("the game exited with code %d"),
                             self->exit_status);

  game_crashed (self, reason);
}

/* The process is gone and both pipes have been read to the end, so the
 * launch is over and the state can be the final one. */
static void
finish_launch (AmlLauncher *self)
{
  if (self->stop_timeout != 0)
    {
      g_source_remove (self->stop_timeout);
      self->stop_timeout = 0;
    }

  /* A game that was asked to stop and did, however it did, has not
     crashed: the force stop reports itself in the log instead. */
  if (self->stop_requested)
    {
      log_line (self, TAG_STATE, "the game stopped");
      set_state (self, AML_LAUNCH_STOPPED, NULL);
      return;
    }

  if (!self->exited_normally || self->exit_status != 0)
    {
      report_how_it_ended (self);
      return;
    }

  {
    g_autofree char *directory = aml_launcher_get_crash_report_directory (self);
    g_autoptr (GFile) reports = g_file_new_for_path (directory);

    if (!g_file_query_exists (reports, NULL))
      {
        set_state (self, AML_LAUNCH_STOPPED, NULL);
        return;
      }

    g_file_enumerate_children_async (
      reports,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
      G_FILE_ATTRIBUTE_TIME_MODIFIED,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, NULL,
      crash_reports_listed, g_object_ref (self));
  }
}

/* ---------------------------------------------------------- *
 * Reading what the game prints
 * ---------------------------------------------------------- */

/* One pipe of the process. It holds the launcher so a line that arrives
 * after the window went away still has somewhere to go. */
typedef struct {
  AmlLauncher *launcher;
  GDataInputStream *stream;
  gboolean stderr;
} AmlLauncherOutput;

static void
output_free (AmlLauncherOutput *output)
{
  g_clear_object (&output->stream);
  g_clear_object (&output->launcher);
  g_free (output);
}

static void read_output_line (AmlLauncherOutput *output);

static void
read_output_line_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  AmlLauncherOutput *output = user_data;
  AmlLauncher *self = output->launcher;
  g_autoptr (GError) error = NULL;
  g_autofree char *line = g_data_input_stream_read_line_finish (
    G_DATA_INPUT_STREAM (source), result, NULL, &error);

  if (line != NULL)
    {
      log_line (self, output->stderr ? TAG_STDERR : TAG_STDOUT, "%s", line);
      read_output_line (output);
      return;
    }

  /* A NULL line without an error is the end of the pipe, which is the
     ordinary way this ends. Anything else is worth one line of its
     own. */
  if (error != NULL)
    log_line (self, TAG_STDOUT, "the output of the game cannot be read: %s",
              error->message);

  /* One reader is done. When the process is gone as well, the launch is
     over and the last lines are already in the log. */
  if (--self->readers == 0 && self->exited)
    finish_launch (self);

  output_free (output);
}

static void
read_output_line (AmlLauncherOutput *output)
{
  g_data_input_stream_read_line_async (output->stream, G_PRIORITY_DEFAULT,
                                       NULL, read_output_line_ready, output);
}

/* Takes the stream it is given over. */
static void
watch_output (AmlLauncher *self, GInputStream *stream, gboolean stderr)
{
  AmlLauncherOutput *output = g_new0 (AmlLauncherOutput, 1);

  output->launcher = g_object_ref (self);
  output->stream = g_data_input_stream_new (stream);
  output->stderr = stderr;

  g_data_input_stream_set_newline_type (output->stream,
                                        G_DATA_STREAM_NEWLINE_TYPE_ANY);

  self->readers++;
  read_output_line (output);
}

/* ---------------------------------------------------------- *
 * The process
 * ---------------------------------------------------------- */

/* The game was asked to stop and did not: it is ended for good. The
 * state still comes out as Stopped, because this stop was asked for and
 * not a crash, but the log says what happened. */
static gboolean
on_stop_timeout (gpointer user_data)
{
  AmlLauncher *self = user_data;

  self->stop_timeout = 0;

  log_line (self, TAG_STATE,
            "the game did not stop within %d seconds, ending it now",
            AML_LAUNCH_STOP_TIMEOUT);

  if (self->process != NULL)
    g_subprocess_force_exit (self->process);

  return G_SOURCE_REMOVE;
}

static void
on_process_exited (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlLauncher *self = user_data;

  /* How it ended is read from the process itself: wait_finish() only
     says whether the exit was a clean one. */
  g_subprocess_wait_finish (G_SUBPROCESS (source), result, NULL);

  self->exited = TRUE;
  self->exited_normally = g_subprocess_get_if_exited (self->process);
  self->terminated_by_signal = g_subprocess_get_if_signaled (self->process);

  if (self->exited_normally)
    self->exit_status = g_subprocess_get_exit_status (self->process);

  if (self->terminated_by_signal)
    self->term_signal = g_subprocess_get_term_sig (self->process);

  g_clear_object (&self->process);

  /* The pipes can still hold the last lines of the game, so the launch
     is over once they have been read. */
  if (self->readers == 0)
    finish_launch (self);

  g_object_unref (self);
}

gboolean
aml_launcher_spawn (AmlLauncher *self, GPtrArray *argv, GError **error)
{
  g_autoptr (GSubprocessLauncher) launcher = NULL;
  g_autoptr (GError) spawn_error = NULL;
  g_autofree char *shown = NULL;
  g_autoptr (GDateTime) now = NULL;
  char **vector = NULL;
  const char *const *arguments = NULL;

  g_return_val_if_fail (AML_IS_LAUNCHER (self), FALSE);
  g_return_val_if_fail (argv != NULL && argv->len > 0, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* The one rule that keeps a game from being started twice: the pipeline
     holds on to the process from the moment it spawns until the moment
     it is gone, and this is the only place a process is started. A
     launch that is still preparing has nothing to refuse yet, because
     this is how a launch ends. */
  if (self->process != NULL)
    {
      g_set_error (error, AML_LAUNCH_ERROR, AML_LAUNCH_ERROR_ALREADY_RUNNING,
                   "this instance is already %s",
                   aml_launch_state_to_string (self->state));
      return FALSE;
    }

  /* GSubprocessLauncher wants a NULL terminated vector, and AML never
     builds a shell line: the arguments go to the process as they are.
     The vector is only the array of pointers: the words themselves
     belong to @argv and are not freed here. */
  vector = g_new0 (char *, argv->len + 1);

  for (guint i = 0; i < argv->len; i++)
    vector[i] = g_ptr_array_index (argv, i);

  arguments = (const char *const *) vector;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_PIPE);

  /* AML runs on Wayland and sets GDK_BACKEND for itself. The game is not
     AML and must not inherit that: it opens its window the way it does
     anywhere else on this desktop, through XWayland. Nothing else about
     the environment is changed, DISPLAY included. */
  g_subprocess_launcher_unsetenv (launcher, "GDK_BACKEND");

  /* The game writes its own files next to the instance: its worlds, its
     logs, its crash reports. */
  g_subprocess_launcher_set_cwd (launcher, self->game_directory);

  set_state (self, AML_LAUNCH_LAUNCHING, "starting the game");

  /* What is about to run, with the token of the session taken out: the
     command line is worth having in the log, the token is not. */
  shown = aml_launch_redact (argv);
  log_line (self, TAG_STATE, "%s", shown);

  self->process =
    g_subprocess_launcher_spawnv (launcher, arguments, &spawn_error);

  g_free (vector);

  if (self->process == NULL)
    {
      launcher_failed (self, spawn_error->message);
      g_propagate_error (error, g_steal_pointer (&spawn_error));
      return FALSE;
    }

  now = g_date_time_new_now_utc ();
  self->started_at = now != NULL ? g_date_time_to_unix (now) : 0;
  self->exited = FALSE;
  self->exited_normally = FALSE;
  self->terminated_by_signal = FALSE;
  self->exit_status = 0;
  self->term_signal = 0;
  self->readers = 0;

  log_line (self, TAG_STATE, "the game runs as process %s",
            g_subprocess_get_identifier (self->process));

  watch_output (self, g_subprocess_get_stdout_pipe (self->process), FALSE);
  watch_output (self, g_subprocess_get_stderr_pipe (self->process), TRUE);

  g_subprocess_wait_async (self->process, NULL, on_process_exited,
                           g_object_ref (self));

  /* The process is up: that is the event the Running state comes from. */
  set_state (self, AML_LAUNCH_RUNNING, NULL);

  return TRUE;
}

/* ---------------------------------------------------------- *
 * The pipeline
 * ---------------------------------------------------------- */

/* What to say when nothing usable for Java was found. The reason is
 * specific: which Java was asked for, which is needed, or that there is
 * none at all. */
static char *
no_java_reason (AmlLauncher *self, const char *instance_java,
                const char *global_java)
{
  int required = self->resolved != NULL ? self->resolved->java_major_version
                                        : 0;
  const char *configured =
    instance_java != NULL && *instance_java != '\0' ? instance_java
                                                     : global_java;

  if (required > 0)
    {
      if (configured != NULL && *configured != '\0')
        return g_strdup_printf (
          "\"%s\" was not found among the detected Java installations, and "
          "no other one on this system runs Minecraft %s, which needs "
          "Java %d or newer",
          configured, self->version, required);

      return g_strdup_printf (
        "no Java on this system runs Minecraft %s, which needs Java %d "
        "or newer", self->version, required);
    }

  if (configured != NULL && *configured != '\0')
    return g_strdup_printf (
      "\"%s\" was not found among the detected Java installations, and "
      "there is no other one to run Minecraft %s with",
      configured, self->version);

  return g_strdup_printf (
    "no Java installation was found on this system to run Minecraft %s "
    "with", self->version);
}

/* The Java a launch runs with: the one the instance asks for, the one the
 * settings ask for, or the newest one that runs what the game needs.
 *
 * @required is the Java the document of the game asks for, which the
 * merged document of a loader names as its Minecraft version does. */
static const AmlJava *
choose_java (AmlLauncher *self, GPtrArray *javas, int required,
             char       **instance_java, const char **global_java)
{
  const AmlJava *java = NULL;

  g_object_get (self->instance, "java-path", instance_java, NULL);

  if (self->settings != NULL)
    *global_java = aml_settings_get_default_java_path (self->settings);
  else
    *global_java = NULL;

  java = aml_java_select (javas, *instance_java, *global_java, required);

  if (java == NULL)
    {
      launcher_failed (self,
                       no_java_reason (self, *instance_java, *global_java));
      return NULL;
    }

  /* A JVM without AWT's window bridge cannot open the window the game
     draws in, so this is worth refusing before the game starts. */
  if (java->headless)
    {
      launcher_failed (self,
                       "the selected Java is a headless one: the game "
                       "needs a Java that can open a window");
      return NULL;
    }

  return java;
}

static void
remember_java (AmlLauncher *self, const AmlJava *java)
{
  g_free (self->java_executable);
  self->java_executable = g_strdup (java->executable);
  self->java_major_version = java->major_version;
}

/* The last step of every launch: the arguments the game is started with,
 * and the game. Everything before this has only been deciding what they
 * will be. */
static void
run_the_game (AmlLauncher *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autofree char *jvm_args = NULL;
  g_autofree char *offline_uuid = NULL;
  AmlAccount *account = NULL;
  AmlLaunchSession session;

  if (self->stop_requested || self->resolved == NULL)
    return;

  /* The active account, read here rather than when the launch was asked
     for: it may be one that was signed in again while the game was still
     being downloaded. */
  account = active_account (self);

  if (account != NULL)
    session = aml_launch_session_for (account);
  else
    {
      /* Nobody signed in: the game plays offline as Player, the name a
         fresh Minecraft falls back to, with the UUID the game derives
         from it so the same player is the same player every time. */
      offline_uuid = aml_account_offline_uuid (AML_LAUNCH_OFFLINE_NAME);
      session.name = AML_LAUNCH_OFFLINE_NAME;
      session.uuid = offline_uuid;
      session.user_type = AML_LAUNCH_OFFLINE_USER_TYPE;
      session.access_token = AML_LAUNCH_OFFLINE_TOKEN;
    }

  g_object_get (self->instance, "jvm-args", &jvm_args, NULL);

  argv = aml_launch_build_arguments (self->resolved, &session,
                                     self->java_executable,
                                     self->game_directory, jvm_args, &error);

  if (argv == NULL)
    {
      launcher_failed (self, error->message);
      return;
    }

  /* Who the game is about to run as. The token is not in this line: the
     command line that is logged by the spawn carries it redacted, and
     this one is about the account. */
  log_line (self, TAG_STATE, "playing as %s (%s)",
            account != NULL ? account->name : AML_LAUNCH_OFFLINE_NAME,
            account != NULL ? aml_account_type_name (account->type)
                            : "Offline");

  log_line (self, TAG_STATE, "Java %d at %s", self->java_major_version,
            self->java_executable);

  /* The spawn reports its own failure as the Failed state; the error is
     only here for a caller that asked for one. */
  aml_launcher_spawn (self, argv, NULL);
}

static void
on_java_detected (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  AmlLauncher *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = aml_java_detect_finish (result, &error);
  g_autofree char *instance_java = NULL;
  const char *global_java = NULL;
  const AmlJava *java = NULL;

  if (javas == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      g_object_unref (self);
      return;
    }

  /* A stop during the Java search ends the launch here: nothing runs
     after it. */
  if (self->stop_requested)
    {
      g_object_unref (self);
      return;
    }

  java = choose_java (self, javas,
                      self->resolved != NULL
                        ? self->resolved->java_major_version
                        : 0,
                      &instance_java, &global_java);

  if (java == NULL)
    {
      g_object_unref (self);
      return;
    }

  remember_java (self, java);
  run_the_game (self);

  g_object_unref (self);
}

static void
on_resolved (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  AmlLauncher *self = user_data;
  g_autoptr (GError) error = NULL;
  AmlResolvedVersion *resolved = aml_resolve_version_finish (result, &error);

  if (resolved == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      g_object_unref (self);
      return;
    }

  if (self->stop_requested)
    {
      aml_resolved_version_free (resolved);
      g_object_unref (self);
      return;
    }

  /* The resolve is where every download of a launch happens, so this is
     the moment the command line starts being built. */
  g_clear_pointer (&self->resolved, aml_resolved_version_free);
  self->resolved = resolved;

  /* A loader that was installed on the way here already chose the Java it
     is installed and run with. */
  if (self->java_executable != NULL)
    {
      run_the_game (self);
      g_object_unref (self);
      return;
    }

  set_state (self, AML_LAUNCH_LAUNCHING, "choosing Java");

  aml_java_detect_async (self->cancellable, on_java_detected,
                         g_object_ref (self));
  g_object_unref (self);
}

/* The resolve reports what it is fetching as it goes, which is where
   the Downloading state and its numbers come from. A version can hold
   thousands of files, so the numbers are only passed on when the
   percentage they add up to changes: a view has no use for five
   thousand state changes that all say 63%. */
static void
on_resolve_progress (const AmlResolveProgress *progress, gpointer user_data)
{
  AmlLauncher *self = user_data;
  guint percent;
  g_autofree char *detail = NULL;

  if (self->stop_requested)
    return;

  if (progress->total > progress->done)
    {
      percent = (progress->done * 100) / progress->total;

      if (percent == self->last_percent)
        return;

      self->last_percent = percent;
      detail = g_strdup_printf ("%s %u%%", progress->stage, percent);
      set_state (self, AML_LAUNCH_DOWNLOADING, detail);
      return;
    }

  if (self->state == AML_LAUNCH_DOWNLOADING)
    set_state (self, AML_LAUNCH_PREPARING, self->version);
}

/* ---------------------------------------------------------- *
 * What the loader of an instance adds to its game
 * ---------------------------------------------------------- */

/* The document of a loader version is a version document of its own that
 * inherits from a Minecraft version, so a loader instance runs that
 * document merged over the one it inherits from: the same merge a
 * profile inherits from its parent. */
typedef struct {
  AmlLauncher *launcher; /* the reference this work holds */
  AmlVersionManifest *manifest;
  AmlLoader loader;
  AmlVersionDescription *profile; /* the document the loader adds */
  char *game_version;
  char *loader_version;
} LoaderRequest;

static void
loader_request_free (LoaderRequest *request)
{
  g_clear_object (&request->manifest);
  g_clear_object (&request->profile);
  g_clear_object (&request->launcher);
  g_free (request->game_version);
  g_free (request->loader_version);
  g_free (request);
}

static void on_install_done (GObject *source, GAsyncResult *result,
                             gpointer user_data);
static void on_install_progress (const char *stage, guint done, guint total,
                                 gpointer user_data);

/* The Java that runs the processors of an install is the Java the game
   runs, so it is chosen once and kept for the launch itself. */
static void
on_java_for_the_install (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                         gpointer user_data)
{
  LoaderRequest *request = user_data;
  AmlLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = aml_java_detect_finish (result, &error);
  g_autofree char *instance_java = NULL;
  const char *global_java = NULL;
  const AmlJava *java = NULL;
  int required = 0;

  if (javas == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  if (self->stop_requested)
    {
      loader_request_free (request);
      return;
    }

  /* The Java version the game asks for is the one the Minecraft document
     names: a loader document of its own does not say. */
  if (request->profile != NULL && request->profile->java_version != NULL)
    required = request->profile->java_version->major_version;

  java = choose_java (self, javas, required, &instance_java, &global_java);

  if (java == NULL)
    {
      loader_request_free (request);
      return;
    }

  remember_java (self, java);
  log_line (self, TAG_STATE, "installing %s %s with Java %d at %s",
            aml_loader_display_name (request->loader), request->loader_version,
            java->major_version, java->executable);

  aml_loaders_install_async (
    request->loader, request->game_version, request->loader_version,
    request->profile, java->executable, NULL, self->cancellable,
    on_install_progress, self, on_install_done, request);
}

static void
on_install_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  LoaderRequest *request = user_data;
  AmlLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlLoaderInstall) install = NULL;
  g_autoptr (AmlVersionDescription) merged = NULL;

  install = aml_loaders_install_finish (result, &error);

  if (install == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  if (self->stop_requested)
    {
      loader_request_free (request);
      return;
    }

  /* What the loader added to the game: its own document over the one of
     the Minecraft version, and the client jar its processors patched. */
  merged = aml_version_description_merge (request->profile, install->description);

  aml_resolve_description_async (merged, merged->id, install->client_jar, NULL,
                                 NULL, NULL, self->cancellable,
                                 on_resolve_progress, self, on_resolved,
                                 g_object_ref (self));
  loader_request_free (request);
}

/* What an install is fetching, which is a handful of files out of the
   loader's own maven: the same numbers as the resolve reports, so the
   state a screen shows is the same shape whichever is running. */
static void
on_install_progress (const char *stage, guint done, guint total,
                     gpointer user_data)
{
  AmlLauncher *self = user_data;
  guint percent;
  g_autofree char *detail = NULL;

  if (self->stop_requested || total == 0)
    return;

  percent = (done * 100) / total;

  if (percent == self->last_percent)
    return;

  self->last_percent = percent;
  detail = g_strdup_printf ("%s %u%%", stage, percent);
  set_state (self, AML_LAUNCH_DOWNLOADING, detail);
}

/* The document of the Minecraft version, for a loader that has to be
 * installed: the install patches its client jar, so it is the document of
 * the game that is wanted first. */
static void
on_game_document (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  LoaderRequest *request = user_data;
  AmlLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  AmlVersionDescription *document = aml_version_load_finish (result, &error);

  if (document == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  request->profile = document;

  if (self->stop_requested)
    {
      loader_request_free (request);
      return;
    }

  set_state (self, AML_LAUNCH_LAUNCHING, "choosing Java");

  aml_java_detect_async (self->cancellable, on_java_for_the_install, request);
}

static void
on_vanilla_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  LoaderRequest *request = user_data;
  AmlLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionDescription) vanilla =
    aml_version_load_finish (result, &error);
  g_autoptr (AmlVersionDescription) merged = NULL;

  if (vanilla == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  merged = aml_version_description_merge (vanilla, request->profile);

  aml_resolve_description_async (merged, merged->id, NULL, NULL, NULL, NULL,
                                 self->cancellable, on_resolve_progress, self,
                                 on_resolved, g_object_ref (self));
  loader_request_free (request);
}

static void
on_profile_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  LoaderRequest *request = user_data;
  AmlLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  AmlVersionDescription *profile =
    aml_loaders_profile_finish (result, &error);

  if (profile == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  request->profile = profile;

  if (self->stop_requested)
    {
      loader_request_free (request);
      return;
    }

  aml_version_load_async (request->manifest, request->game_version,
                          self->cancellable, on_vanilla_loaded, request);
}

static void
resolve_with_a_loader (AmlLauncher        *self,
                       AmlVersionManifest *manifest,
                       AmlLoader           loader,
                       const char         *loader_version)
{
  LoaderRequest *request = g_new0 (LoaderRequest, 1);

  request->launcher = g_object_ref (self);
  request->manifest = g_object_ref (manifest);
  request->loader = loader;
  request->game_version = g_strdup (self->version);
  request->loader_version = g_strdup (loader_version);

  /* A loader that publishes an installer is installed before the game it
     runs is resolved: its processors patch the client jar the game then
     starts from, so the document of the Minecraft version comes first. */
  if (aml_loader_publishes_an_installer (loader))
    {
      log_line (self, TAG_STATE, "installing %s %s for %s",
                aml_loader_display_name (loader), loader_version,
                self->version != NULL ? self->version : "");

      aml_version_load_async (manifest, request->game_version,
                              self->cancellable, on_game_document, request);
      return;
    }

  log_line (self, TAG_STATE, "asking %s for %s %s",
            aml_loader_display_name (loader), loader_version,
            self->version != NULL ? self->version : "");

  aml_loaders_profile_async (loader, request->game_version, loader_version,
                             self->cancellable, on_profile_loaded, request);
}

static void
on_manifest_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  AmlLauncher *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionManifest) manifest =
    aml_version_manifest_fetch_finish (result, &error);

  if (manifest == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      g_object_unref (self);
      return;
    }

  if (self->stop_requested)
    {
      g_object_unref (self);
      return;
    }

  {
    g_autofree char *loader_version = NULL;
    AmlLoader loader = AML_LOADER_VANILLA;

    g_object_get (self->instance, "loader", &loader, "loader-version",
                  &loader_version, NULL);

    if (loader != AML_LOADER_VANILLA)
      {
        /* An instance that names a loader this build does not install, or
           that has no loader version, is a broken instance rather than a
           game to run: saying so is better than running the wrong thing. */
        if (!aml_loader_is_supported (loader))
          launcher_failed (self, "this build of AML does not install "
                                 "that loader");
        else if (loader_version == NULL || *loader_version == '\0')
          launcher_failed (self, "the instance has no loader version");
        else
          resolve_with_a_loader (self, manifest, loader, loader_version);

        g_object_unref (self);
        return;
      }
  }

  aml_resolve_version_async (manifest, self->version, NULL, NULL, NULL,
                             self->cancellable, on_resolve_progress, self,
                             on_resolved, g_object_ref (self));
  g_object_unref (self);
}

/* ---------------------------------------------------------- *
 * Starting and stopping
 * ---------------------------------------------------------- */

/* The first request of a launch: the version manifest, and from there
 * everything a launch needs before the game can start. It is a function
 * of its own because an account that had to be signed in again carries
 * on from here with the token it just got. */
static void
start_preparing (AmlLauncher *self)
{
  log_line (self, TAG_STATE, "preparing Minecraft %s",
            self->version != NULL ? self->version : "this instance");

  aml_version_manifest_fetch_async (AML_VERSION_MANIFEST_URL,
                                    self->cancellable, on_manifest_ready,
                                    g_object_ref (self));
}

/* A sign-in that has to happen again, said as a state of its own: nothing
 * is running, nothing is broken, and the account is what the user has to
 * take care of. A token that cannot be traded for a new one is dropped
 * from the store, so that the account says it needs a sign-in wherever it
 * is shown instead of pretending to be ready. */
static void
launcher_needs_sign_in (AmlLauncher *self, const char *reason)
{
  AmlAccount *account = active_account (self);
  g_autofree char *detail = NULL;

  if (account != NULL)
    {
      AmlAccount *without = aml_account_copy (account);
      g_clear_pointer (&without->access_token, g_free);
      g_clear_pointer (&without->refresh_token, g_free);
      without->expires_at = 0;

      /* A store that cannot be written is not a launch that is broken:
         the state still says what the user has to do. */
      aml_accounts_update (self->accounts, without, NULL);
      aml_account_free (without);
    }

  detail = g_strdup_printf ("%s: open the users menu and sign in as \"%s\" "
                            "again",
                            reason,
                            account != NULL && account->name != NULL
                              ? account->name
                              : "the account");

  log_line (self, TAG_STATE, "%s", detail);
  set_state (self, AML_LAUNCH_NEEDS_SIGN_IN, detail);
}

/* A new token for an account whose old one has run out. */
static void
on_account_refreshed (GObject      *source G_GNUC_UNUSED,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr (AmlLauncher) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlAccount) account = NULL;

  account = aml_auth_refresh_finish (result, &error);

  if (self->stop_requested)
    return;

  if (account == NULL)
    {
      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          log_line (self, TAG_STATE,
                    "the launch was cancelled before the game started");
          set_state (self, AML_LAUNCH_STOPPED, NULL);
          return;
        }

      launcher_needs_sign_in (
          self, error != NULL ? error->message : "the account could not be "
                                                     "signed in again");
      return;
    }

  /* The refreshed token is what the rest of the launch runs with, and it
     is stored so that the next launch does not have to ask again. */
  if (!aml_accounts_update (self->accounts, account, &error))
    log_line (self, TAG_STATE,
              "the refreshed token could not be stored: %s", error->message);

  start_preparing (self);
}

/* The account a launch runs as. Nobody signed in is not a reason to
 * refuse: the game starts offline as Player. An account whose token has
 * run out is signed in again before anything is downloaded, because a
 * game that is going to need a token later can fail now in words
 * instead. */
static gboolean
account_for_launch (AmlLauncher *self)
{
  AmlAccount *account = active_account (self);

  if (account == NULL)
    return TRUE;

  if (account->type == AML_ACCOUNT_OFFLINE)
    return TRUE;

  if (aml_account_token_is_valid (account, g_get_real_time () / 1000000))
    return TRUE;

  log_line (self, TAG_STATE, "signing \"%s\" in again before launching",
            account->name != NULL ? account->name : "the account");

  aml_auth_refresh_async (account, self->cancellable, on_account_refreshed,
                          g_object_ref (self));

  return FALSE;
}

void
aml_launcher_start (AmlLauncher *self)
{
  /* Not g_autofree: the instance hands the string over and it becomes the
     version of this launch, which outlives this function. */
  char *version = NULL;

  g_return_if_fail (AML_IS_LAUNCHER (self));

  /* The state is the whole answer to "is this instance already being
     launched": a second launch is refused instead of racing the first. */
  if (aml_launch_state_is_busy (self->state))
    {
      log_line (self, TAG_STATE,
                "Minecraft %s is already %s, nothing was started",
                self->version != NULL ? self->version : "",
                aml_launch_state_to_string (self->state));
      return;
    }

  g_object_get (self->instance, "minecraft-version", &version, NULL);

  g_free (self->version);
  self->version = version;

  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
  self->stop_requested = FALSE;
  self->last_percent = 0;

  /* The Java of the launch before this one says nothing about this one:
     a loader that has to be installed first chooses its own. */
  g_clear_pointer (&self->java_executable, g_free);
  self->java_major_version = 0;

  set_state (self, AML_LAUNCH_PREPARING, self->version);

  /* Without an account there is nobody to play as, and nothing to
     download for. An account that has to be signed in again is signed in
     again first, and carries on from here when it has one. */
  if (!account_for_launch (self))
    return;

  start_preparing (self);
}

void
aml_launcher_stop (AmlLauncher *self)
{
  g_return_if_fail (AML_IS_LAUNCHER (self));

  if (!aml_launch_state_is_busy (self->state))
    return;

  self->stop_requested = TRUE;

  /* Nothing is running yet: what has to stop is the resolve and the
     Java search behind it. */
  if (self->process == NULL)
    {
      log_line (self, TAG_STATE, "stopping before the game starts");
      g_cancellable_cancel (self->cancellable);
      set_state (self, AML_LAUNCH_STOPPED, NULL);
      return;
    }

  set_state (self, AML_LAUNCH_STOPPING, "asking the game to stop");
  log_line (self, TAG_STATE, "asking the game to stop");

  /* Minecraft has its own way of ending; SIGTERM is what every launcher
     sends first, and a game that hangs is stopped for good after the
     timeout. */
  g_subprocess_send_signal (self->process, SIGTERM);

  if (self->stop_timeout == 0)
    self->stop_timeout = g_timeout_add_seconds (AML_LAUNCH_STOP_TIMEOUT,
                                                on_stop_timeout, self);
}

/* ---------------------------------------------------------- *
 * The object itself
 * ---------------------------------------------------------- */

AmlInstance *
aml_launcher_get_instance (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), NULL);

  return self->instance;
}

AmlLaunchState
aml_launcher_get_state (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), AML_LAUNCH_STOPPED);

  return self->state;
}

const char *
aml_launcher_get_state_detail (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), NULL);

  return self->detail;
}

const char *
aml_launcher_get_game_directory (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), NULL);

  return self->game_directory;
}

char *
aml_launcher_get_log_directory (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), NULL);

  return g_build_filename (self->game_directory, "logs", NULL);
}

char *
aml_launcher_get_crash_report_directory (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), NULL);

  return g_build_filename (self->game_directory, "crash-reports", NULL);
}

AmlLauncher *
aml_launcher_new (AmlInstance *instance,
                  AmlSettings *settings,
                  AmlAccounts *accounts)
{
  AmlLauncher *self;

  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (settings == NULL || AML_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (accounts == NULL || AML_IS_ACCOUNTS (accounts), NULL);

  self = g_object_new (AML_TYPE_LAUNCHER, NULL);
  self->instance = g_object_ref (instance);

  if (settings != NULL)
    self->settings = g_object_ref (settings);

  if (accounts != NULL)
    self->accounts = g_object_ref (accounts);

  g_object_get (instance, "directory", &self->game_directory, NULL);

  return self;
}

AmlAccounts *
aml_launcher_get_accounts (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), NULL);

  return self->accounts;
}

static void
aml_launcher_dispose (GObject *object)
{
  AmlLauncher *self = AML_LAUNCHER (object);

  /* Downloads in flight stop when the owner lets go of the launcher.
     The game itself is not killed: the readers and the wait hold a
     reference for as long as it runs. */
  g_cancellable_cancel (self->cancellable);

  G_OBJECT_CLASS (aml_launcher_parent_class)->dispose (object);
}

static void
aml_launcher_finalize (GObject *object)
{
  AmlLauncher *self = AML_LAUNCHER (object);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->process);
  g_clear_pointer (&self->resolved, aml_resolved_version_free);
  g_clear_object (&self->instance);
  g_clear_object (&self->settings);
  g_clear_object (&self->accounts);
  g_clear_pointer (&self->lines, g_ptr_array_unref);
  g_free (self->detail);
  g_free (self->version);
  g_free (self->game_directory);
  g_free (self->java_executable);

  G_OBJECT_CLASS (aml_launcher_parent_class)->finalize (object);
}

static void
aml_launcher_class_init (AmlLauncherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_launcher_dispose;
  object_class->finalize = aml_launcher_finalize;

  /* g_cclosure_marshal_generic marshals these: the state is an enum and
     the other arguments are strings, all of which it can copy. */
  aml_launcher_signals[AML_LAUNCHER_SIGNAL_STATE] =
    g_signal_new ("state", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 2, AML_TYPE_LAUNCH_STATE,
                  G_TYPE_STRING);

  aml_launcher_signals[AML_LAUNCHER_SIGNAL_OUTPUT] =
    g_signal_new ("output", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING,
                  G_TYPE_BOOLEAN);
}

static void
aml_launcher_init (AmlLauncher *self)
{
  self->lines = g_ptr_array_new_with_free_func (g_free);
  self->state = AML_LAUNCH_STOPPED;
}
