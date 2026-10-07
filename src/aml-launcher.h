/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlLauncher: putting one instance on the air and following it.
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
 *  - the object, which owns the subprocess, reads its two pipes line by
 *    line with a timestamp and holds the state. The state only ever
 *    changes on something that really happened: progress from the
 *    resolve, a process that spawned, a process that exited.
 *
 * The object belongs to the instance it was made for and refuses a
 * second launch while one is in flight, so the same game never runs
 * twice.
 *
 * A launch runs as the active account of aml-accounts.h: its name, its
 * UUID, its access token and the user type that goes with the kind of
 * account it is. An account whose token has run out is refreshed before
 * the launch starts, and one that cannot be refreshed leaves the launch
 * in the Needs sign-in state, which is a state a user can act on rather
 * than a game that will not start. */

#ifndef AML_LAUNCHER_H
#define AML_LAUNCHER_H

#include <gio/gio.h>

#include "aml-accounts.h"
#include "aml-instance.h"
#include "aml-minecraft-resolve.h"
#include "aml-settings.h"

G_BEGIN_DECLS

/* The name the game is told about through ${launcher_name}. */
#define AML_LAUNCHER_NAME "AML"

/* ${launcher_version}. AML has no release number yet, so this is what
 * the game is told instead of one that was invented. */
#define AML_LAUNCHER_VERSION "dev"

/* What the game is told about an offline session: a token it cannot
 * authenticate with, and the user type that says the session is no real
 * account. A Microsoft session is told "msa" and a token of its own. */
#define AML_LAUNCH_OFFLINE_TOKEN "0"
#define AML_LAUNCH_OFFLINE_USER_TYPE "legacy"
#define AML_LAUNCH_MICROSOFT_USER_TYPE "msa"

/* Who a game with nobody signed in plays as: the name a fresh Minecraft
 * falls back to, and the one its offline UUID is derived from. */
#define AML_LAUNCH_OFFLINE_NAME "Player"

/* The window the game opens in, which is what ${resolution_width} and
 * ${resolution_height} mean. */
#define AML_LAUNCH_WIDTH 854
#define AML_LAUNCH_HEIGHT 480

/* Seconds a game gets to end by itself after a SIGTERM before it is
 * stopped for good. */
#define AML_LAUNCH_STOP_TIMEOUT 5

#define AML_LAUNCH_ERROR (aml_launch_error_quark ())

typedef enum {
  AML_LAUNCH_ERROR_NO_JAVA,       /* nothing usable for this version */
  AML_LAUNCH_ERROR_NO_MAIN_CLASS, /* the document says no main class */
  AML_LAUNCH_ERROR_ARGUMENTS,     /* the command line cannot be built */
  AML_LAUNCH_ERROR_ALREADY_RUNNING /* the game is already up */
} AmlLaunchError;

GQuark aml_launch_error_quark (void);

/* ---------------------------------------------------------- *
 * The parts that are only functions
 * ---------------------------------------------------------- */

/* What a launch is doing, and what it did. A state only ever comes from
 * something that really happened. */
typedef enum {
  AML_LAUNCH_PREPARING,        /* the manifest, the resolve and the Java */
  AML_LAUNCH_DOWNLOADING,      /* the resolve is fetching files */
  AML_LAUNCH_LAUNCHING,        /* the command line is being built and run */
  AML_LAUNCH_RUNNING,          /* the game is up */
  AML_LAUNCH_STOPPING,         /* a stop was asked for and not finished */
  AML_LAUNCH_STOPPED,          /* the game ended by itself, or was stopped */
  AML_LAUNCH_CRASHED,          /* it ended with a failure */
  AML_LAUNCH_FAILED,           /* it never started */
  AML_LAUNCH_NEEDS_SIGN_IN     /* the account has to sign in again */
} AmlLaunchState;

/* The type of the state, which the "state" signal carries. */
#define AML_TYPE_LAUNCH_STATE (aml_launch_state_get_type ())
GType aml_launch_state_get_type (void);

/* The same string the enum member spells: "Preparing", "Downloading",
 * "Launching", "Running", "Stopping", "Stopped", "Crashed", "Failed" or
 * "Needs sign-in". Never NULL. */
const char *aml_launch_state_to_string (AmlLaunchState state);

/* TRUE for the states a launch passes through and lives in: everything
 * from Preparing to Stopping. A game in any of them is a game that must
 * not be started a second time. A launch that needs a sign-in is not
 * busy: nothing is running and the user can start it again. */
gboolean aml_launch_state_is_busy (AmlLaunchState state);

/* What a launch runs as: the account, in the four words the game is
 * told about the player. */
typedef struct {
  const char *name;
  const char *uuid;         /* dashed, as the account carries it */
  const char *access_token; /* an offline session gets the token it
                             * cannot use, so the argument is never
                             * missing */
  const char *user_type;    /* "msa" or "legacy" */
} AmlLaunchSession;

/* The session of an account: its own name and UUID, its token, and the
 * user type that says which kind of account it is. Every member points
 * into @account, which the caller keeps. */
AmlLaunchSession aml_launch_session_for (const AmlAccount *account);

/* @value with every ${name} @placeholders holds replaced by the value
 * for that name. A name the table does not hold is left exactly as the
 * document wrote it, because a launcher that does not know a
 * placeholder must not silently drop it. (transfer full) */
char *aml_launch_substitute (const char  *value,
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
 * directory, and @session, which is what the player is called, who they
 * are and the token the game authenticates with. Both the modern
 * "arguments" object and the legacy "minecraftArguments" line arrive in
 * @resolved, so both end up the same way here.
 *
 * @resolved is what aml_resolve_version_finish() returned and is not
 * modified, @session what aml_launch_session_for() returned, @java_executable
 * the java this launch runs with, @game_directory the directory the game
 * runs in, which is also the ${game_directory} of the arguments, and
 * @jvm_args the line the instance stores.
 *
 * Fails with AML_LAUNCH_ERROR_NO_MAIN_CLASS when the document names no
 * main class and with AML_LAUNCH_ERROR_ARGUMENTS when the JVM arguments
 * of the instance do not parse as a command line. (transfer full) */
GPtrArray *aml_launch_build_arguments (const AmlResolvedVersion *resolved,
                                      const AmlLaunchSession   *session,
                                      const char             *java_executable,
                                      const char             *game_directory,
                                      const char             *jvm_args,
                                      GError                **error);

/* The command line as one string, with every argument that carries a
 * secret replaced by "***": --accessToken and its value, and anything
 * that follows a --password or a --token. This is the only form of a
 * command line AML ever shows, so a token cannot reach a log. Arguments
 * are quoted the way a shell would, so what is printed is what could be
 * pasted into a terminal. (transfer full) */
char *aml_launch_redact (GPtrArray *argv);

/* ---------------------------------------------------------- *
 * The launch itself
 * ---------------------------------------------------------- */

#define AML_TYPE_LAUNCHER (aml_launcher_get_type ())

G_DECLARE_FINAL_TYPE (AmlLauncher, aml_launcher, AML, LAUNCHER, GObject)

/* "state" carries the new state and the detail that goes with it, which
 * is what a view shows next to it: "assets 12/340" while downloading,
 * the reason a launch failed, and NULL when there is nothing to add.
 *
 * "line" is one line of the log as it is shown, already timestamped and
 * tagged. @tag is "stdout", "stderr" or "state": the first two come
 * from the game, the last one from AML itself. */
enum {
  AML_LAUNCHER_SIGNAL_STATE,
  AML_LAUNCHER_SIGNAL_OUTPUT,
  AML_LAUNCHER_N_SIGNALS
};

/* An instance to launch, the settings that decide how much of its log is
 * kept and whether a crash opens the log folder, and the accounts a
 * launch runs as. @settings may be NULL in a test that does not care
 * about them, and so may @accounts, in which case there is no account to
 * play as and a launch says so. */
AmlLauncher *aml_launcher_new (AmlInstance  *instance,
                               AmlSettings  *settings,
                               AmlAccounts  *accounts);

/* The accounts a launch runs as, or NULL when the launcher was made
 * without any. Borrowed. */
AmlAccounts *aml_launcher_get_accounts (AmlLauncher *self);

/* The instance this launcher belongs to. Borrowed. */
AmlInstance *aml_launcher_get_instance (AmlLauncher *self);

AmlLaunchState aml_launcher_get_state (AmlLauncher *self);

/* What goes with the state: the numbers while downloading, the reason a
 * launch failed, NULL when there is nothing to add. Borrowed. */
const char *aml_launcher_get_state_detail (AmlLauncher *self);

/* The directory the game runs in and writes its own files to. Borrowed
 * from the instance. */
const char *aml_launcher_get_game_directory (AmlLauncher *self);

/* <game directory>/logs, where the game writes its own log, and
 * <game directory>/crash-reports, where it writes what it wrote about
 * itself the last time it died. (transfer full) */
char *aml_launcher_get_log_directory (AmlLauncher *self);
char *aml_launcher_get_crash_report_directory (AmlLauncher *self);

/* Starts a launch: the manifest, the resolve, the Java and the process.
 *
 * Does nothing but a line in the log when the instance is already busy,
 * which is what keeps a second launch of the same instance from ever
 * happening. A launch that cannot finish ends in the Failed state with
 * the reason in the log, never with an error, so a caller has nothing
 * to do but watch the state. */
void aml_launcher_start (AmlLauncher *self);

/* Asks a running game to end: SIGTERM first, and when it is still up
 * after AML_LAUNCH_STOP_TIMEOUT seconds it is stopped for good. A
 * launch that did not reach the process yet is cancelled where it is.
 * A launcher that is not busy does nothing. */
void aml_launcher_stop (AmlLauncher *self);

/* The last step of a launch: runs @argv, java included as its first
 * element, with the instance directory as the working directory and
 * without the GDK_BACKEND AML sets for itself.
 *
 * This is public because it is the one step a headless test can drive on
 * its own with a fake java: the state machine, the two output readers
 * and the stop are then exactly the ones a real launch goes through.
 * Fails with AML_LAUNCH_ERROR_ALREADY_RUNNING while a process of this
 * instance is up, and ends in the Failed state with the reason in the
 * log when the process cannot be started at all. */
gboolean aml_launcher_spawn (AmlLauncher *self,
                             GPtrArray    *argv,
                             GError      **error);

/* How many lines the log keeps before the oldest are dropped, which is
 * what the settings say, and at least one. */
guint aml_launcher_get_line_limit (AmlLauncher *self);

/* The number of lines the log holds right now. */
guint aml_launcher_get_line_count (AmlLauncher *self);

/* Every line logged so far, oldest first, as one string with "\n"
 * between the lines and none at the end. (transfer full) */
char *aml_launcher_get_log_text (AmlLauncher *self);

/* Drops every line logged so far. */
void aml_launcher_clear_log (AmlLauncher *self);

G_END_DECLS

#endif /* AML_LAUNCHER_H */