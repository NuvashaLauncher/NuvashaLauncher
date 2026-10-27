/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncher: putting one instance on the air and following it.
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
 * A launch runs as the active account of nuvashalauncher-accounts.h: its name, its
 * UUID, its access token and the user type that goes with the kind of
 * account it is. An account whose token has run out is refreshed before
 * the launch starts, and one that cannot be refreshed leaves the launch
 * in the Needs sign-in state, which is a state a user can act on rather
 * than a game that will not start. */

#ifndef nuvashalauncher_LAUNCHER_H
#define nuvashalauncher_LAUNCHER_H

#include <gio/gio.h>

#include "nuvashalauncher-accounts.h"
#include "nuvashalauncher-instance.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-settings.h"

G_BEGIN_DECLS

/* The name the game is told about through ${launcher_name}. */
#define nuvashalauncher_LAUNCHER_NAME "nuvashalauncher"

/* ${launcher_version}. Keep the value aligned with the application release. */
#define nuvashalauncher_LAUNCHER_VERSION nuvashalauncher_VERSION

/* Values passed to the game for offline and Microsoft sessions. */
#define nuvashalauncher_LAUNCH_OFFLINE_TOKEN "0"
#define nuvashalauncher_LAUNCH_OFFLINE_USER_TYPE "legacy"
#define nuvashalauncher_LAUNCH_MICROSOFT_USER_TYPE "msa"

/* Default name used for offline sessions and UUID generation. */
#define nuvashalauncher_LAUNCH_OFFLINE_NAME "Player"

/* Default values for ${resolution_width} and ${resolution_height}. */
#define nuvashalauncher_LAUNCH_WIDTH 854
#define nuvashalauncher_LAUNCH_HEIGHT 480

/* Seconds a game gets to end by itself after a SIGTERM before it is
 * stopped for good. */
#define nuvashalauncher_LAUNCH_STOP_TIMEOUT 5

#define nuvashalauncher_LAUNCH_ERROR (nuvashalauncher_launch_error_quark ())

typedef enum {
  nuvashalauncher_LAUNCH_ERROR_NO_JAVA,       /* nothing usable for this version */
  nuvashalauncher_LAUNCH_ERROR_NO_MAIN_CLASS, /* the document says no main class */
  nuvashalauncher_LAUNCH_ERROR_ARGUMENTS,     /* the command line cannot be built */
  nuvashalauncher_LAUNCH_ERROR_ALREADY_RUNNING /* the game is already up */
} nuvashalauncherLaunchError;

GQuark nuvashalauncher_launch_error_quark (void);

/* Current or final state of a launch. */
typedef enum {
  nuvashalauncher_LAUNCH_PREPARING,        /* the manifest, the resolve and the Java */
  nuvashalauncher_LAUNCH_DOWNLOADING,      /* the resolve is fetching files */
  nuvashalauncher_LAUNCH_LAUNCHING,        /* the command line is being built and run */
  nuvashalauncher_LAUNCH_RUNNING,          /* the game is up */
  nuvashalauncher_LAUNCH_STOPPING,         /* a stop was asked for and not finished */
  nuvashalauncher_LAUNCH_STOPPED,          /* the game ended by itself, or was stopped */
  nuvashalauncher_LAUNCH_CRASHED,          /* it ended with a failure */
  nuvashalauncher_LAUNCH_FAILED,            /* launch preparation failed */
  nuvashalauncher_LAUNCH_NEEDS_SIGN_IN     /* the account has to sign in again */
} nuvashalauncherLaunchState;

/* The type of the state, which the "state" signal carries. */
#define nuvashalauncher_TYPE_LAUNCH_STATE (nuvashalauncher_launch_state_get_type ())
GType nuvashalauncher_launch_state_get_type (void);

/* Returns the stable display name of @state. */
const char *nuvashalauncher_launch_state_to_string (nuvashalauncherLaunchState state);

/* TRUE for the states a launch passes through and lives in: everything
 * from Preparing to Stopping. A game in any of them is a game that must
 * not be started a second time. A launch that needs a sign-in is not
 * busy: nothing is running and the user can start it again. */
gboolean nuvashalauncher_launch_state_is_busy (nuvashalauncherLaunchState state);

/* Account values passed to the game. */
typedef struct {
  const char *name;
  const char *uuid;         /* dashed, as the account carries it */
  const char *access_token; /* an offline session gets the token it
                             * cannot use, so the argument is never
                             * missing */
  const char *user_type;    /* "msa" or "legacy" */
} nuvashalauncherLaunchSession;

/* The session of an account: its own name and UUID, its token, and the
 * user type that says which kind of account it is. Every member points
 * into @account, which the caller keeps. */
nuvashalauncherLaunchSession nuvashalauncher_launch_session_for (const nuvashalauncherAccount *account);

/* Replace each ${name} placeholder in @value with its corresponding value.
 * A name the table does not hold is left exactly as the
 * document wrote it, because a launcher that does not know a
 * unknown placeholders are preserved. (transfer full) */
char *nuvashalauncher_launch_substitute (const char  *value,
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
  * @resolved is returned by nuvashalauncher_resolve_version_finish() and is not
  * modified, @session is returned by nuvashalauncher_launch_session_for(), @java_executable
 * the java this launch runs with, @game_directory the directory the game
 * runs in, which is also the ${game_directory} of the arguments, and
 * @jvm_args the line the instance stores.
 *
 * Fails with nuvashalauncher_LAUNCH_ERROR_NO_MAIN_CLASS when the document names no
 * main class and with nuvashalauncher_LAUNCH_ERROR_ARGUMENTS when the JVM arguments
 * of the instance do not parse as a command line. (transfer full) */
GPtrArray *nuvashalauncher_launch_build_arguments (const nuvashalauncherResolvedVersion *resolved,
                                      const nuvashalauncherLaunchSession   *session,
                                      const char             *java_executable,
                                      const char             *game_directory,
                                      const char             *jvm_args,
                                      GError                **error);

/* The command line as one string, with every argument that carries a
 * secret replaced by "***": --accessToken and its value, and anything
 * that follows a --password or a --token. This is the only form of a
 * command line nuvashalauncher ever shows, so a token cannot reach a log. Arguments
 * are shell-quoted so the result can be pasted into a terminal. (transfer full) */
char *nuvashalauncher_launch_redact (GPtrArray *argv);

/* ---------------------------------------------------------- *
 * The launch itself
 * ---------------------------------------------------------- */

#define nuvashalauncher_TYPE_LAUNCHER (nuvashalauncher_launcher_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncher, nuvashalauncher_launcher, nuvashalauncher, LAUNCHER, GObject)

/* "state" carries the new state and the detail that goes with it, which
 * is what a view shows next to it: "assets 12/340" while downloading,
 * the reason a launch failed, and NULL when there is nothing to add.
 *
 * "line" is one line of the log as it is shown, already timestamped and
 * tagged. @tag is "stdout", "stderr" or "state": the first two come
 * from the game, the last one from nuvashalauncher itself. */
enum {
  nuvashalauncher_LAUNCHER_SIGNAL_STATE,
  nuvashalauncher_LAUNCHER_SIGNAL_OUTPUT,
  nuvashalauncher_LAUNCHER_N_SIGNALS
};

/* An instance to launch, the settings that decide how much of its log is
 * kept and whether a crash opens the log folder, and the accounts a
 * launch runs as. @settings may be NULL in a test that does not care
 * about them, and so may @accounts, in which case there is no account to
 * play as and a launch says so. */
nuvashalauncher *nuvashalauncher_launcher_new (nuvashalauncherInstance  *instance,
                               nuvashalauncherSettings  *settings,
                               nuvashalauncherAccounts  *accounts);

/* The accounts a launch runs as, or NULL when the launcher was made
 * without any. Borrowed. */
nuvashalauncherAccounts *nuvashalauncher_launcher_get_accounts (nuvashalauncher *self);

/* Select an account only for this launch; does not change the saved active
 * account.  @account_id may be an account id, UUID, or name. */
gboolean nuvashalauncher_launcher_set_account (nuvashalauncher *self,
                                       const char *account_id,
                                       GError **error);

/* The instance this launcher belongs to. Borrowed. */
nuvashalauncherInstance *nuvashalauncher_launcher_get_instance (nuvashalauncher *self);

nuvashalauncherLaunchState nuvashalauncher_launcher_get_state (nuvashalauncher *self);

/* Optional state detail: progress while downloading or a failure reason.
 * Borrowed. */
const char *nuvashalauncher_launcher_get_state_detail (nuvashalauncher *self);

/* The directory the game runs in and writes its own files to. Borrowed
 * from the instance. */
const char *nuvashalauncher_launcher_get_game_directory (nuvashalauncher *self);

/* Return the game's log and crash-report directories. (transfer full) */
char *nuvashalauncher_launcher_get_log_directory (nuvashalauncher *self);
char *nuvashalauncher_launcher_get_crash_report_directory (nuvashalauncher *self);

/* Starts a launch: the manifest, the resolve, the Java and the process.
 *
 * Does nothing but a line in the log when the instance is already busy,
 * which is what keeps a second launch of the same instance from ever
 * happening. A launch that cannot finish ends in the Failed state with
 * the reason in the log, never with an error, so a caller has nothing
 * to do but watch the state. */
void nuvashalauncher_launcher_start (nuvashalauncher *self);

/* Asks a running game to end: SIGTERM first, and when it is still up
 * after nuvashalauncher_LAUNCH_STOP_TIMEOUT seconds it is stopped for good. A
 * launch that did not reach the process yet is cancelled where it is.
 * A launcher that is not busy does nothing. */
void nuvashalauncher_launcher_stop (nuvashalauncher *self);

/* The last step of a launch: runs @argv, java included as its first
 * element, with the instance directory as the working directory and
 * without a GDK_BACKEND selection from nuvashalauncher.
 *
 * This is public because it is the one step a headless test can drive on
 * its own with a fake java: the state machine, the two output readers
 * and the stop are then exactly the ones a real launch goes through.
 * Fails with nuvashalauncher_LAUNCH_ERROR_ALREADY_RUNNING while a process of this
 * instance is up, and ends in the Failed state with the reason in the
 * log when the process cannot be started at all. */
gboolean nuvashalauncher_launcher_spawn (nuvashalauncher *self,
                             GPtrArray    *argv,
                             GError      **error);

/* Maximum number of retained log lines. */
guint nuvashalauncher_launcher_get_line_limit (nuvashalauncher *self);

/* The number of lines the log holds right now. */
guint nuvashalauncher_launcher_get_line_count (nuvashalauncher *self);

char *nuvashalauncher_launcher_get_log_text (nuvashalauncher *self);

/* Drops every line logged so far. */
void nuvashalauncher_launcher_clear_log (nuvashalauncher *self);

G_END_DECLS

#endif /* nuvashalauncher_LAUNCHER_H */
