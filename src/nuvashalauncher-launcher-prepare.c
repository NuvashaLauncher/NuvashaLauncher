/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-launcher-private.h"
#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-instance-components.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-settings.h"

#include <string.h>

/* ---------------------------------------------------------- *
 * The pipeline
 * ---------------------------------------------------------- */

static void
set_log_private_values (NuvashaLauncher *self, const NuvashaLauncherLaunchSession *session)
{
  g_clear_pointer (&self->log_access_token, g_free);
  g_clear_pointer (&self->log_session, g_free);
  g_clear_pointer (&self->log_uuid, g_free);

  if (session == NULL)
    return;

  if (session->access_token != NULL &&
      g_strcmp0 (session->access_token, NUVASHALAUNCHER_LAUNCH_OFFLINE_TOKEN) != 0)
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
const NuvashaLauncherJava *
choose_java (NuvashaLauncher *self, GPtrArray *javas, int required,
             char       **instance_java)
{
  const NuvashaLauncherJava *java = NULL;

  g_object_get (self->instance, "java-path", instance_java, NULL);
  if (*instance_java != NULL && **instance_java != '\0' &&
      !(g_file_test (*instance_java, G_FILE_TEST_IS_REGULAR) &&
        g_file_test (*instance_java, G_FILE_TEST_IS_EXECUTABLE)))
    {
      nuvashalauncher_instance_set_string (self->instance, "java-path", NULL, NULL);
      g_clear_pointer (instance_java, g_free);
    }

  java = nuvashalauncher_java_select (javas, *instance_java, required);

  if (java == NULL)
    {
      g_autofree char *reason = g_strdup (_("No compatible Java runtime was found."));
      launcher_failed (self, reason);
      return NULL;
    }

  /* A JVM without AWT's window bridge cannot open the window the game
     draws in, so this is worth refusing before the game starts. */
  if (java->headless)
    {
      launcher_failed (self,
                       _("Select a Java runtime with desktop support, not a headless installation."));
      return NULL;
    }

  return java;
}

void
remember_java (NuvashaLauncher *self, const NuvashaLauncherJava *java)
{
  g_free (self->preparation.java_executable);
  self->preparation.java_executable = g_strdup (java->executable);
  self->preparation.java_major_version = java->major_version;
}

/* The last step of every launch: the arguments the game is started with,
 * and the game. Everything before this has only been deciding what they
 * will be. */
static void
run_the_game (NuvashaLauncher *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) argv = NULL;
  g_autofree char *jvm_args = NULL;
  g_autofree char *offline_uuid = NULL;
  NuvashaLauncherAccount *account = NULL;
  NuvashaLauncherLaunchSession session;

  if (self->process.stop_requested || self->preparation.resolved == NULL)
    return;

  /* The active account, read here rather than when the launch was asked
     for: it may be one that was signed in again while the game was still
     being downloaded. */
  account = active_account (self);

  if (account != NULL)
    session = nuvashalauncher_launch_session_for (account);
  else
    {
      /* Nobody signed in: the game plays offline as Player, the name a
         fresh Minecraft falls back to, with the UUID the game derives
         from it so the same player is the same player every time. */
      offline_uuid = nuvashalauncher_account_offline_uuid (NUVASHALAUNCHER_LAUNCH_OFFLINE_NAME);
      session.name = NUVASHALAUNCHER_LAUNCH_OFFLINE_NAME;
      session.uuid = offline_uuid;
      session.user_type = NUVASHALAUNCHER_LAUNCH_OFFLINE_USER_TYPE;
      session.access_token = NUVASHALAUNCHER_LAUNCH_OFFLINE_TOKEN;
    }

  set_log_private_values (self, &session);

  guint memory_mib = 0;
  guint game_width = 0;
  guint game_height = 0;
  gint game_fullscreen = -1;
  gint use_dedicated_gpu = -1;
  gint use_gamemode = -1;
  gint use_mangohud = -1;
  g_autoptr (NuvashaLauncherSettings) settings = nuvashalauncher_settings_new ();
  const char *effective_jvm_args = NULL;
  guint effective_width = 0;
  guint effective_height = 0;
  gboolean effective_fullscreen = FALSE;
  gboolean effective_gpu = FALSE;
  gboolean effective_gamemode = FALSE;
  gboolean effective_mangohud = FALSE;

  g_object_get (self->instance,
                "jvm-args", &jvm_args,
                "memory-mib", &memory_mib,
                "game-width", &game_width,
                "game-height", &game_height,
                "game-fullscreen", &game_fullscreen,
                "use-dedicated-gpu", &use_dedicated_gpu,
                "use-gamemode", &use_gamemode,
                "use-mangohud", &use_mangohud,
                NULL);

  if (memory_mib == 0)
    memory_mib = nuvashalauncher_settings_get_default_memory_mib (settings);

  effective_jvm_args = (jvm_args != NULL && *jvm_args != '\0')
                         ? jvm_args
                         : nuvashalauncher_settings_get_default_jvm_args (settings);

  effective_width = game_width > 0
                      ? game_width
                      : nuvashalauncher_settings_get_default_game_width (settings);
  effective_height = game_height > 0
                       ? game_height
                       : nuvashalauncher_settings_get_default_game_height (settings);
  effective_fullscreen = game_fullscreen >= 0
                           ? (game_fullscreen != 0)
                           : nuvashalauncher_settings_get_default_game_fullscreen (settings);
  effective_gpu = use_dedicated_gpu >= 0
                    ? (use_dedicated_gpu != 0)
                    : nuvashalauncher_settings_get_default_use_dedicated_gpu (settings);
  effective_gamemode = use_gamemode >= 0
                         ? (use_gamemode != 0)
                         : nuvashalauncher_settings_get_default_use_gamemode (settings);
  effective_mangohud = use_mangohud >= 0
                         ? (use_mangohud != 0)
                         : nuvashalauncher_settings_get_default_use_mangohud (settings);

  self->preparation.use_dedicated_gpu = effective_gpu;

  argv = nuvashalauncher_launch_build_arguments_with_memory (
                                     self->preparation.resolved, &session,
                                     self->preparation.java_executable,
                                     self->game_directory, effective_jvm_args,
                                     memory_mib, &error);

  if (argv == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      return;
    }

  if (effective_fullscreen)
    {
      gboolean has_fullscreen = FALSE;
      for (guint i = 0; i < argv->len; i++)
        if (g_strcmp0 (g_ptr_array_index (argv, i), "--fullscreen") == 0)
          {
            has_fullscreen = TRUE;
            break;
          }
      if (!has_fullscreen)
        g_ptr_array_add (argv, g_strdup ("--fullscreen"));
    }
  else if (effective_width > 0 && effective_height > 0)
    {
      gboolean has_width = FALSE;
      gboolean has_height = FALSE;
      for (guint i = 0; i < argv->len; i++)
        {
          const char *arg = g_ptr_array_index (argv, i);
          if (g_strcmp0 (arg, "--width") == 0)
            has_width = TRUE;
          else if (g_strcmp0 (arg, "--height") == 0)
            has_height = TRUE;
        }
      if (!has_width && !has_height)
        {
          g_ptr_array_add (argv, g_strdup ("--width"));
          g_ptr_array_add (argv, g_strdup_printf ("%u", effective_width));
          g_ptr_array_add (argv, g_strdup ("--height"));
          g_ptr_array_add (argv, g_strdup_printf ("%u", effective_height));
        }
    }

  {
    const char *qp_world =
      g_object_get_data (G_OBJECT (self), "nuvashalauncher-quick-play-world");
    const char *qp_server =
      g_object_get_data (G_OBJECT (self), "nuvashalauncher-quick-play-server");

    if (qp_world != NULL && *qp_world != '\0')
      {
        g_ptr_array_add (argv, g_strdup ("--quickPlaySingleplayer"));
        g_ptr_array_add (argv, g_strdup (qp_world));
      }
    else if (qp_server != NULL && *qp_server != '\0')
      {
        g_ptr_array_add (argv, g_strdup ("--quickPlayMultiplayer"));
        g_ptr_array_add (argv, g_strdup (qp_server));
      }

    g_object_set_data (G_OBJECT (self), "nuvashalauncher-quick-play-world", NULL);
    g_object_set_data (G_OBJECT (self), "nuvashalauncher-quick-play-server", NULL);
  }

  /* Optional performance wrappers: gamemoderun wraps mangohud which wraps java. */
  if (effective_mangohud)
    {
      g_autofree char *mangohud_bin = g_find_program_in_path ("mangohud");

      if (mangohud_bin != NULL)
        g_ptr_array_insert (argv, 0, g_steal_pointer (&mangohud_bin));
    }

  if (effective_gamemode)
    {
      g_autofree char *gamemode_bin = g_find_program_in_path ("gamemoderun");

      if (gamemode_bin != NULL)
        g_ptr_array_insert (argv, 0, g_steal_pointer (&gamemode_bin));
    }

  /* Who the game is about to run as. The token is not in this line: the
     command line that is logged by the spawn carries it redacted, and
     this one is about the account. */
  log_line (self, TAG_STATE, "playing as %s (%s)",
            account != NULL ? account->name : NUVASHALAUNCHER_LAUNCH_OFFLINE_NAME,
            account != NULL ? nuvashalauncher_account_type_name (account->type)
                            : "Offline");

  log_line (self, TAG_STATE, "Java %d at %s", self->preparation.java_major_version,
            self->preparation.java_executable);

  /* The spawn reports its own failure as the Failed state; the error is
     only here for a caller that asked for one. */
  nuvashalauncher_launcher_spawn (self, argv, NULL);
}

/* The Java search of a launch: the launcher, which a stop can take away
   while the search runs, and the handle the answer is meant to release. */
typedef struct {
  NuvashaLauncher *launcher;
  NuvashaLauncherTaskScope *operation;
  GPtrArray *javas;
} JavaSearch;

static void
java_search_free (JavaSearch *search)
{
  g_clear_object (&search->launcher);
  g_clear_pointer (&search->javas, g_ptr_array_unref);

  if (search->operation != NULL)
    nuvashalauncher_task_scope_unref (search->operation);

  g_free (search);
}

static void
on_custom_java_probed (GObject *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer user_data)
{
  JavaSearch *search = user_data;
  NuvashaLauncher *self = search->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherJava) custom =
    nuvashalauncher_java_probe_finish (result, &error);
  g_autofree char *instance_java = NULL;
  const NuvashaLauncherJava *java = NULL;

  if (self->process.stop_requested ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      java_search_free (search);
      return;
    }

  if (custom == NULL)
    {
      nuvashalauncher_instance_set_string (self->instance, "java-path", NULL, NULL);
    }
  else
    {
      g_ptr_array_add (search->javas, g_steal_pointer (&custom));
    }

  java = choose_java (self, search->javas,
                      self->preparation.resolved != NULL
                        ? self->preparation.resolved->java_major_version : 0,
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

static void
on_java_detected (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  JavaSearch *search = user_data;
  NuvashaLauncher *self = search->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = nuvashalauncher_java_detect_finish (result, &error);
  g_autofree char *instance_java = NULL;
  const NuvashaLauncherJava *java = NULL;

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

  g_object_get (self->instance, "java-path", &instance_java, NULL);
  if (instance_java != NULL && *instance_java != '\0' &&
      !(g_file_test (instance_java, G_FILE_TEST_IS_REGULAR) &&
        g_file_test (instance_java, G_FILE_TEST_IS_EXECUTABLE)))
    {
      nuvashalauncher_instance_set_string (self->instance, "java-path", NULL, NULL);
      g_clear_pointer (&instance_java, g_free);
    }

  if (instance_java != NULL && *instance_java != '\0')
    {
      /* Explicit runtimes selected through Flatpak portals are not part of
       * the host-runtime scan. Probe that configured executable directly and
       * add its result to the candidates before selecting Java. */
      nuvashalauncher_task_scope_unref (search->operation);
      search->operation = NULL;
      search->javas = g_steal_pointer (&javas);
      search->operation = nuvashalauncher_java_probe_async (
        instance_java, on_custom_java_probed, search);
      nuvashalauncher_task_scope_follow_cancellable (search->operation,
                                             self->preparation.cancellable);
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
  NuvashaLauncher *self = NUVASHALAUNCHER_LAUNCHER (user_data);
  g_autoptr (GError) error = NULL;
  NuvashaLauncherResolvedVersion *resolved = nuvashalauncher_resolve_version_finish (result, &error);

  if (self->preparation.download != NULL)
    {
      nuvashalauncher_download_finish (self->preparation.download);
      g_clear_object (&self->preparation.download);
    }

  if (resolved == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      g_object_unref (self);
      return;
    }

  if (self->process.stop_requested)
    {
      nuvashalauncher_resolved_version_free (resolved);
      g_object_unref (self);
      return;
    }

  /* The resolve is where every download of a launch happens, so this is
     the moment the command line starts being built. */
  g_clear_pointer (&self->preparation.resolved, nuvashalauncher_resolved_version_free);
  self->preparation.resolved = resolved;

  /* A loader that was installed on the way here already chose the Java it
     is installed and run with. */
  if (self->preparation.java_executable != NULL)
    {
      run_the_game (self);
      g_object_unref (self);
      return;
    }

  set_state (self, NUVASHALAUNCHER_LAUNCH_LAUNCHING, "choosing Java");

  {
    g_autofree char *instance_java = NULL;
    JavaSearch *search = g_new0 (JavaSearch, 1);

    search->launcher = g_object_ref (self);
    g_object_get (self->instance, "java-path", &instance_java, NULL);

    if (instance_java != NULL && *instance_java != '\0')
      {
        if (g_file_test (instance_java, G_FILE_TEST_IS_REGULAR) &&
            g_file_test (instance_java, G_FILE_TEST_IS_EXECUTABLE))
          {
            search->javas = g_ptr_array_new_with_free_func (
              (GDestroyNotify) nuvashalauncher_java_free);
            search->operation = nuvashalauncher_java_probe_async (
              instance_java, on_custom_java_probed, search);
          }
        else
          {
            nuvashalauncher_instance_set_string (self->instance, "java-path",
                                                 NULL, NULL);
            search->operation = nuvashalauncher_java_detect_async (
              on_java_detected, search);
          }
      }
    else
      {
        search->operation = nuvashalauncher_java_detect_async (
          on_java_detected, search);
      }

    nuvashalauncher_task_scope_follow_cancellable (search->operation,
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
on_resolve_progress (const NuvashaLauncherResolveProgress *progress, gpointer user_data)
{
  NuvashaLauncher *self = NUVASHALAUNCHER_LAUNCHER (user_data);
  guint percent;
  g_autofree char *detail = NULL;

  if (self->process.stop_requested)
    return;

  if (progress->stage != NULL &&
      g_strcmp0 (self->process.last_stage, progress->stage) != 0)
    {
      g_free (self->process.last_stage);
      self->process.last_stage = g_strdup (progress->stage);
      self->process.last_percent = G_MAXUINT;
      log_line (self, TAG_STATE, "downloading %s for Minecraft %s",
                progress->stage, self->version != NULL ? self->version : "");
      if (self->preparation.download != NULL)
        nuvashalauncher_download_set_detail (
          self->preparation.download,
          g_strcmp0 (progress->stage, "assets") == 0
            ? _("Downloading assets…")
            : _("Downloading dependencies…"));
    }

  if (self->preparation.download != NULL)
    nuvashalauncher_download_set_progress (self->preparation.download,
                                           progress->done, progress->total);

  if (progress->total > progress->done)
    {
      percent = (progress->done * 100) / progress->total;

      if (percent == self->process.last_percent)
        return;

      self->process.last_percent = percent;
      detail = g_strdup_printf ("%s %u%%", progress->stage, percent);
      set_state (self, NUVASHALAUNCHER_LAUNCH_DOWNLOADING, detail);
      return;
    }

  if (self->state == NUVASHALAUNCHER_LAUNCH_DOWNLOADING)
    set_state (self, NUVASHALAUNCHER_LAUNCH_PREPARING, self->version);
}
