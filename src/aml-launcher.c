/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlLauncher prepares and runs one instance, then owns its process state.
 * The preparation pipeline is asynchronous; the pure command-line helpers
 * remain usable without a JVM or network and are covered independently by
 * tests.
 *
 * The game must not inherit AML's GDK_BACKEND setting. Other environment
 * variables are left unchanged. */

#include "aml-launcher.h"
#include "aml-i18n.h"

#include "aml-auth-microsoft.h"
#include "aml-instance-components.h"
#include "aml-java.h"
#include "aml-line-reader.h"
#include "aml-loaders.h"
#include "aml-minecraft-resolve.h"
#include "aml-minecraft-versions.h"

#include <signal.h>
#include <string.h>

/* The three kinds of line the log holds. */
GQuark
aml_launch_error_quark (void)
{
  return g_quark_from_static_string ("aml-launch-error");
}

/* ---------------------------------------------------------- *
 * The object
 * ---------------------------------------------------------- */

#include "aml-launcher-private.h"

G_DEFINE_FINAL_TYPE (AmlLauncher, aml_launcher, G_TYPE_OBJECT)

guint aml_launcher_signals[AML_LAUNCHER_N_SIGNALS];


/* The account a launch plays as, or NULL when there is none. A launcher
 * made without a store is a launcher with no account, which is what a
 * test that drives one step at a time is. */
 AmlAccount *
active_account (AmlLauncher *self)
{
  if (self->accounts == NULL)
    return NULL;

  return aml_accounts_get_active (self->accounts);
}

/* ---------------------------------------------------------- *
 * The state
 * ---------------------------------------------------------- */

/* The state and the detail it goes with go out together, and only when
 * one of them actually changed: a resolve reports progress many times a
 * second and a view has no use for the same line twice. */
void
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

void
launcher_failed (AmlLauncher *self, const char *reason)
{
  log_line (self, TAG_STATE, "%s", reason);
  set_state (self, AML_LAUNCH_FAILED, reason);
}

/* A launch that could not finish because the caller cancelled it, which
 * is what a stop does before the game starts, is not a failure. */
void
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
 * The pipeline
 * ---------------------------------------------------------- */

/* What to say when nothing usable for Java was found. The reason is
 * specific: which Java was asked for, which is needed, or that there is
 * none at all. */
static char *
no_java_reason (AmlLauncher *self, const char *instance_java)
{
  int required = self->resolved != NULL ? self->resolved->java_major_version
                                        : 0;

  if (required > 0)
    {
      if (instance_java != NULL && *instance_java != '\0')
        return g_strdup_printf (
          "\"%s\" was not found among the detected Java installations, and "
          "no other one on this system runs Minecraft %s, which needs "
          "Java %d or newer",
          instance_java, self->version, required);

      return g_strdup_printf (
        "no Java on this system runs Minecraft %s, which needs Java %d "
        "or newer", self->version, required);
    }

  if (instance_java != NULL && *instance_java != '\0')
    return g_strdup_printf (
      "\"%s\" was not found among the detected Java installations, and "
      "there is no other one to run Minecraft %s with",
      instance_java, self->version);

  return g_strdup_printf (
    "no Java installation was found on this system to run Minecraft %s "
    "with", self->version);
}

/* The Java a launch runs with: the one the instance asks for, or the
 * closest compatible one when it asks for none.
 *
 * @required is the Java the document of the game asks for, which the
 * merged document of a loader names as its Minecraft version does. */
const AmlJava *
choose_java (AmlLauncher *self, GPtrArray *javas, int required,
             char       **instance_java)
{
  const AmlJava *java = NULL;

  g_object_get (self->instance, "java-path", instance_java, NULL);

  java = aml_java_select (javas, *instance_java, required);

  if (java == NULL)
    {
      launcher_failed (self, no_java_reason (self, *instance_java));
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

void
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

  if (self->process.stop_requested || self->resolved == NULL)
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
  const AmlJava *java = NULL;

  if (javas == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      g_object_unref (self);
      return;
    }

  /* A stop during the Java search ends the launch here: nothing runs
     after it. */
  if (self->process.stop_requested)
    {
      g_object_unref (self);
      return;
    }

  java = choose_java (self, javas,
                      self->resolved != NULL
                        ? self->resolved->java_major_version
                        : 0,
                      &instance_java);

  if (java == NULL)
    {
      g_object_unref (self);
      return;
    }

  remember_java (self, java);
  run_the_game (self);

  g_object_unref (self);
}

void
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

  if (self->process.stop_requested)
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
void
on_resolve_progress (const AmlResolveProgress *progress, gpointer user_data)
{
  AmlLauncher *self = user_data;
  guint percent;
  g_autofree char *detail = NULL;

  if (self->process.stop_requested)
    return;

  if (progress->total > progress->done)
    {
      percent = (progress->done * 100) / progress->total;

      if (percent == self->process.last_percent)
        return;

      self->process.last_percent = percent;
      detail = g_strdup_printf ("%s %u%%", progress->stage, percent);
      set_state (self, AML_LAUNCH_DOWNLOADING, detail);
      return;
    }

  if (self->state == AML_LAUNCH_DOWNLOADING)
    set_state (self, AML_LAUNCH_PREPARING, self->version);
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
  g_clear_object (&self->process.process);
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
