/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncher: putting one instance on the air and following it.
 *
 * Three parts that can be looked at one by one:
 *
 *  - the pure ones, which are functions over data that is already in
 *    memory: the placeholder substitution of a version document, the
 *    offline UUID, the whole command line of a launch and the printable
 *    form of that command line with the token taken out of it. The
 *    tests use them without a JVM and without a network.
 *
 *  - the pipeline, which is asynchronous from end to end: the version
 *    manifest, the resolve module putting every jar, asset and native on
 *    disk, the Java detection and finally GSubprocess. Nothing here
 *    waits for a socket, a disk or a process in the GTK main loop.
 *
 *  - the object, which owns the subprocess and its state. Process output is
 *    read by the small line-reader helper and handed back here for logging.
 *    The state only changes on something that really happened: progress
 *    from the resolve, a process that spawned, or a process that exited.
 *
 * The object belongs to the instance it was made for and refuses a
 * second launch while one is in flight, so the same game never runs
 * twice.
 *
 * A launch runs as the active account of nuvasha-accounts.h: its name, its
 * UUID, its access token and the user type that goes with the kind of
 * account it is. An account whose token has run out is refreshed before
 * the launch starts, and one that cannot be refreshed leaves the launch
 * in the Needs sign-in state, which is a state a user can act on rather
 * than a game that will not start. */

#ifndef nuvasha_LAUNCHER_H
#define nuvasha_LAUNCHER_H

#include <gio/gio.h>

#include "nuvasha-accounts.h"
#include "nuvasha-instance.h"
#include "nuvasha-minecraft-resolve.h"
#include "nuvasha-settings.h"

G_BEGIN_DECLS

/* The name the game is told about through ${launcher_name}. */
#define nuvasha_LAUNCHER_NAME "nuvasha"

/* ${launcher_version}. Keep the value aligned with the application release. */
#define nuvasha_LAUNCHER_VERSION nuvasha_VERSION

/* Values passed to the game for offline and Microsoft sessions. */
#define nuvasha_LAUNCH_OFFLINE_TOKEN "0"
#define nuvasha_LAUNCH_OFFLINE_USER_TYPE "legacy"
#define nuvasha_LAUNCH_MICROSOFT_USER_TYPE "msa"

/* Default name used for offline sessions and UUID generation. */
#define nuvasha_LAUNCH_OFFLINE_NAME "Player"

/* Default values for ${resolution_width} and ${resolution_height}. */
#define nuvasha_LAUNCH_WIDTH 854
#define nuvasha_LAUNCH_HEIGHT 480

/* Seconds a game gets to end by itself after a SIGTERM before it is
 * stopped for good. */
#define nuvasha_LAUNCH_STOP_TIMEOUT 5

#define nuvasha_LAUNCH_ERROR (nuvasha_launch_error_quark ())

typedef enum {
  nuvasha_LAUNCH_ERROR_NO_JAVA,       /* nothing usable for this version */
  nuvasha_LAUNCH_ERROR_NO_MAIN_CLASS, /* the document says no main class */
  nuvasha_LAUNCH_ERROR_ARGUMENTS,     /* the command line cannot be built */
  nuvasha_LAUNCH_ERROR_ALREADY_RUNNING /* the game is already up */
} NuvashaLaunchError;

GQuark nuvasha_launch_error_quark (void);

/* ---------------------------------------------------------- *
 * The parts that are only functions
 * ---------------------------------------------------------- */

/* Current or final state of a launch. */
typedef enum {
  nuvasha_LAUNCH_PREPARING,        /* the manifest, the resolve and the Java */
  nuvasha_LAUNCH_DOWNLOADING,      /* the resolve is fetching files */
  nuvasha_LAUNCH_LAUNCHING,        /* the command line is being built and run */
  nuvasha_LAUNCH_RUNNING,          /* the game is up */
  nuvasha_LAUNCH_STOPPING,         /* a stop was asked for and not finished */
  nuvasha_LAUNCH_STOPPED,          /* the game ended by itself, or was stopped */
  nuvasha_LAUNCH_CRASHED,          /* it ended with a failure */
  nuvasha_LAUNCH_FAILED,           /* it never started */
  nuvasha_LAUNCH_NEEDS_SIGN_IN     /* the account has to sign in again */
} NuvashaLaunchState;

/* The type of the state, which the "state" signal carries. */
#define nuvasha_TYPE_LAUNCH_STATE (nuvasha_launch_state_get_type ())
GType nuvasha_launch_state_get_type (void);

/* The same string the enum member spells: "Preparing", "Downloading",
 * "Launching", "Running", "Stopping", "Stopped", "Crashed", "Failed" or
 * "Needs sign-in". Never NULL. */
const char *nuvasha_launch_state_to_string (NuvashaLaunchState state);

/* TRUE for the states a launch passes through and lives in: everything
 * from Preparing to Stopping. A game in any of them is a game that must
 * not be started a second time. A launch that needs a sign-in is not
 * busy: nothing is running and the user can start it again. */
gboolean nuvasha_launch_state_is_busy (NuvashaLaunchState state);

/* Account values passed to the game. */
typedef struct {
  const char *name;
  const char *uuid;         /* dashed, as the account carries it */
  const char *access_token; /* an offline session gets the token it
                             * cannot use, so the argument is never
                             * missing */
  const char *user_type;    /* "msa" or "legacy" */
} NuvashaLaunchSession;

/* The session of an account: its own name and UUID, its token, and the
 * user type that says which kind of account it is. Every member points
 * into @account, which the caller keeps. */
NuvashaLaunchSession nuvasha_launch_session_for (const NuvashaAccount *account);

/* Replace each ${name} placeholder in @value with its corresponding value.
 * A name the table does not hold is left exactly as the
 * document wrote it, because a launcher that does not know a
 * placeholder must not silently drop it. (transfer full) */
char *nuvasha_launch_substitute (const char  *value,
                             GHashTable  *placeholders);

/* The whole command line of one launch, as an argv array java takes:
 *
 *   <java_executable>
 *   the jvm arguments of the version document
 *   the jvm arguments of the instance, split like a shell line
 *   -Djava.library.path=<natives>
 *   -cp <classpath>
 *   the logging argument, when the document has one
 *   <main class>
 *   the game arguments of the document
 *
 * The placeholders the document wrote are replaced by the values of this
 * machine: the paths the resolve module handed over, the instance
  * directory, and @session, which contains the player name, UUID, and token.
  * Both the modern
 * "arguments" object and the legacy "minecraftArguments" line arrive in
 * @resolved, so both end up the same way here.
 *
  * @resolved is returned by nuvasha_resolve_version_finish() and is not
  * modified, @session is returned by nuvasha_launch_session_for(), @java_executable
 * the java this launch runs with, @game_directory the directory the game
 * runs in, which is also the ${game_directory} of the arguments, and
 * @jvm_args the line the instance stores.
 *
 * Fails with nuvasha_LAUNCH_ERROR_NO_MAIN_CLASS when the document names no
 * main class and with nuvasha_LAUNCH_ERROR_ARGUMENTS when the JVM arguments
 * of the instance do not parse as a command line. (transfer full) */
GPtrArray *nuvasha_launch_build_arguments (const NuvashaResolvedVersion *resolved,
                                      const NuvashaLaunchSession   *session,
                                      const char             *java_executable,
                                      const char             *game_directory,
                                      const char             *jvm_args,
                                      GError                **error);

/* The command line as one string, with every argument that carries a
 * secret replaced by "***": --accessToken and its value, and anything
 * that follows a --password or a --token. This is the only form of a
 * command line nuvasha ever shows, so a token cannot reach a log. Arguments
 * are shell-quoted so the result can be pasted into a terminal. (transfer full) */
char *nuvasha_launch_redact (GPtrArray *argv);

/* ---------------------------------------------------------- *
 * The launch itself
 * ---------------------------------------------------------- */

#define nuvasha_TYPE_LAUNCHER (nuvasha_launcher_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLauncher, nuvasha_launcher, nuvasha, LAUNCHER, GObject)

/* "state" carries the new state and the detail that goes with it, which
 * is what a view shows next to it: "assets 12/340" while downloading,
 * the reason a launch failed, and NULL when there is nothing to add.
 *
 * "line" is one line of the log as it is shown, already timestamped and
 * tagged. @tag is "stdout", "stderr" or "state": the first two come
 * from the game, the last one from nuvasha itself. */
enum {
  nuvasha_LAUNCHER_SIGNAL_STATE,
  nuvasha_LAUNCHER_SIGNAL_OUTPUT,
  nuvasha_LAUNCHER_N_SIGNALS
};

/* An instance to launch, the settings that decide how much of its log is
 * kept and whether a crash opens the log folder, and the accounts a
 * launch runs as. @settings may be NULL in a test that does not care
 * about them, and so may @accounts, in which case there is no account to
 * play as and a launch says so. */
NuvashaLauncher *nuvasha_launcher_new (NuvashaInstance  *instance,
                               NuvashaSettings  *settings,
                               NuvashaAccounts  *accounts);

/* The accounts a launch runs as, or NULL when the launcher was made
 * without any. Borrowed. */
NuvashaAccounts *nuvasha_launcher_get_accounts (NuvashaLauncher *self);

/* The instance this launcher belongs to. Borrowed. */
NuvashaInstance *nuvasha_launcher_get_instance (NuvashaLauncher *self);

NuvashaLaunchState nuvasha_launcher_get_state (NuvashaLauncher *self);

/* Optional state detail: progress while downloading or a failure reason.
 * Borrowed. */
const char *nuvasha_launcher_get_state_detail (NuvashaLauncher *self);

/* The directory the game runs in and writes its own files to. Borrowed
 * from the instance. */
const char *nuvasha_launcher_get_game_directory (NuvashaLauncher *self);

/* Return the game's log and crash-report directories. (transfer full) */
char *nuvasha_launcher_get_log_directory (NuvashaLauncher *self);
char *nuvasha_launcher_get_crash_report_directory (NuvashaLauncher *self);

/* Starts a launch: the manifest, the resolve, the Java and the process.
 *
 * Does nothing but a line in the log when the instance is already busy,
 * which is what keeps a second launch of the same instance from ever
 * happening. A launch that cannot finish ends in the Failed state with
 * the reason in the log, never with an error, so a caller has nothing
 * to do but watch the state. */
void nuvasha_launcher_start (NuvashaLauncher *self);

/* Asks a running game to end: SIGTERM first, and when it is still up
 * after nuvasha_LAUNCH_STOP_TIMEOUT seconds it is stopped for good. A
 * launch that did not reach the process yet is cancelled where it is.
 * A launcher that is not busy does nothing. */
void nuvasha_launcher_stop (NuvashaLauncher *self);

/* The last step of a launch: runs @argv, java included as its first
 * element, with the instance directory as the working directory and
 * without a GDK_BACKEND selection from nuvasha.
 *
 * This is public because it is the one step a headless test can drive on
 * its own with a fake java: the state machine, the two output readers
 * and the stop are then exactly the ones a real launch goes through.
 * Fails with nuvasha_LAUNCH_ERROR_ALREADY_RUNNING while a process of this
 * instance is up, and ends in the Failed state with the reason in the
 * log when the process cannot be started at all. */
gboolean nuvasha_launcher_spawn (NuvashaLauncher *self,
                             GPtrArray    *argv,
                             GError      **error);

/* Maximum number of retained log lines. */
guint nuvasha_launcher_get_line_limit (NuvashaLauncher *self);

/* The number of lines the log holds right now. */
guint nuvasha_launcher_get_line_count (NuvashaLauncher *self);

/* Every line logged so far, oldest first, as one string with "\n"
 * between the lines and none at the end. (transfer full) */
char *nuvasha_launcher_get_log_text (NuvashaLauncher *self);

/* Drops every line logged so far. */
void nuvasha_launcher_clear_log (NuvashaLauncher *self);

G_END_DECLS

#endif /* nuvasha_LAUNCHER_H */
