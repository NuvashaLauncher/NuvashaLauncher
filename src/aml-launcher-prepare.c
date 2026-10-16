/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-launcher-private.h"
#include "aml-auth-microsoft.h"
#include "aml-instance-components.h"
#include "aml-i18n.h"
#include "aml-java.h"
#include "aml-loaders.h"
#include "aml-minecraft-resolve.h"
#include "aml-minecraft-versions.h"
#include "aml-sandbox.h"

#include <string.h>

/* ---------------------------------------------------------- *
 * The pipeline
 * ---------------------------------------------------------- */

/* What to say when nothing usable for Java was found. */
static char *
no_java_reason (void)
{
  return g_strdup (_("No compatible Java runtime was found."));
}

static void
set_log_private_values (AmlLauncher *self, const AmlLaunchSession *session)
{
  g_clear_pointer (&self->log_access_token, g_free);
  g_clear_pointer (&self->log_session, g_free);
  g_clear_pointer (&self->log_uuid, g_free);

  if (session == NULL)
    return;

  if (session->access_token != NULL &&
      g_strcmp0 (session->access_token, AML_LAUNCH_OFFLINE_TOKEN) != 0)
    {
      self->log_access_token = g_strdup (session->access_token);
      self->log_session = g_strdup_printf ("token:%s:%s",
                                           session->access_token,
                                           session->uuid != NULL
                                             ? session->uuid : "");
    }

  if (session->uuid != NULL)
    self->log_uuid = g_strdup (session->uuid);
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
      g_autofree char *reason = no_java_reason ();
      launcher_failed (self, reason);
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
  g_free (self->preparation.java_executable);
  self->preparation.java_executable = g_strdup (java->executable);
  self->preparation.java_major_version = java->major_version;
}

/* The last step of every launch: the arguments the game is started with,
 * and the game. Everything before this has only been deciding what they
 * will be. */
static void
run_the_game (AmlLauncher *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GPtrArray) sandbox_argv = NULL;
  g_autofree char *jvm_args = NULL;
  g_autofree char *offline_uuid = NULL;
  AmlAccount *account = NULL;
  AmlLaunchSession session;

  if (self->process.stop_requested || self->preparation.resolved == NULL)
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

  set_log_private_values (self, &session);

  g_object_get (self->instance, "jvm-args", &jvm_args, NULL);

  argv = aml_launch_build_arguments (self->preparation.resolved, &session,
                                     self->preparation.java_executable,
                                     self->game_directory, jvm_args, &error);

  if (argv == NULL)
    {
      launcher_failed (self, error->message);
      return;
    }

  if (self->settings != NULL &&
      aml_settings_get_sandbox_enabled (self->settings))
    {
      sandbox_argv = aml_sandbox_wrap_arguments (
        argv, self->preparation.resolved, self->preparation.java_executable,
        self->game_directory, &error);

      if (sandbox_argv == NULL)
        {
          launcher_failed (self, error->message);
          return;
        }

      g_clear_pointer (&argv, g_ptr_array_unref);
      argv = g_steal_pointer (&sandbox_argv);
      log_line (self, TAG_STATE, "bubblewrap sandbox enabled");
    }

  /* Who the game is about to run as. The token is not in this line: the
     command line that is logged by the spawn carries it redacted, and
     this one is about the account. */
  log_line (self, TAG_STATE, "playing as %s (%s)",
            account != NULL ? account->name : AML_LAUNCH_OFFLINE_NAME,
            account != NULL ? aml_account_type_name (account->type)
                            : "Offline");

  log_line (self, TAG_STATE, "Java %d at %s", self->preparation.java_major_version,
            self->preparation.java_executable);

  /* The spawn reports its own failure as the Failed state; the error is
     only here for a caller that asked for one. */
  aml_launcher_spawn (self, argv, NULL);
}

/* The Java search of a launch: the launcher, which a stop can take away
   while the search runs, and the handle the answer is meant to release. */
typedef struct {
  AmlLauncher *launcher;
  AmlTaskScope *operation;
} JavaSearch;

static void
java_search_free (JavaSearch *search)
{
  g_clear_object (&search->launcher);

  if (search->operation != NULL)
    aml_task_scope_unref (search->operation);

  g_free (search);
}

static void
on_java_detected (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  JavaSearch *search = user_data;
  AmlLauncher *self = search->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = aml_java_detect_finish (result, &error);
  g_autofree char *instance_java = NULL;
  const AmlJava *java = NULL;

  if (javas == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      java_search_free (search);
      return;
    }

  /* A stop during the Java search ends the launch here: nothing runs
     after it. */
  if (self->process.stop_requested)
    {
      java_search_free (search);
      return;
    }

  java = choose_java (self, javas,
                      self->preparation.resolved != NULL
                        ? self->preparation.resolved->java_major_version
                        : 0,
                      &instance_java);

  if (java == NULL)
    {
      java_search_free (search);
      return;
    }

  remember_java (self, java);
  run_the_game (self);

  java_search_free (search);
}

void
on_resolved (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
             gpointer user_data)
{
  AmlLauncher *self = AML_LAUNCHER (user_data);
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
  g_clear_pointer (&self->preparation.resolved, aml_resolved_version_free);
  self->preparation.resolved = resolved;

  /* A loader that was installed on the way here already chose the Java it
     is installed and run with. */
  if (self->preparation.java_executable != NULL)
    {
      run_the_game (self);
      g_object_unref (self);
      return;
    }

  set_state (self, AML_LAUNCH_LAUNCHING, "choosing Java");

  {
    JavaSearch *search = g_new0 (JavaSearch, 1);

    search->launcher = g_object_ref (self);
    search->operation = aml_java_detect_async (on_java_detected, search);
    aml_task_scope_follow_cancellable (search->operation,
                                       self->preparation.cancellable);
  }

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
  AmlLauncher *self = AML_LAUNCHER (user_data);
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
