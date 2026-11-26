/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-launcher-private.h"
#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-instance-components.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-i18n.h"

/* Internal loader-install/profile pipeline. */

typedef struct {
  NuvashaLauncher *launcher; /* the reference this work holds */
  NuvashaLauncherVersionManifest *manifest;
  NuvashaLauncherLoader loader;
  NuvashaLauncherVersionDescription *profile; /* the document the loader adds */
  char *game_version;
  char *loader_version;

  /* The operation this request is waiting on, released with it. */
  NuvashaLauncherTaskScope *operation;
} LoaderRequest;

static void
loader_request_free (LoaderRequest *request)
{
  g_clear_object (&request->manifest);
  g_clear_object (&request->profile);
  g_clear_object (&request->launcher);

  if (request->operation != NULL)
    nuvashalauncher_task_scope_unref (request->operation);

  g_free (request->game_version);
  g_free (request->loader_version);
  g_free (request);
}

typedef struct {
  NuvashaLauncher *launcher;
  NuvashaLauncherVersionDescription *merged;
  char *client_jar;
  NuvashaLauncherTaskScope *operation;
  GPtrArray *javas;
} ResolveJavaCheck;

static void
resolve_java_check_free (ResolveJavaCheck *check)
{
  if (check == NULL)
    return;

  g_clear_object (&check->launcher);
  g_clear_object (&check->merged);
  g_free (check->client_jar);
  g_clear_pointer (&check->javas, g_ptr_array_unref);
  if (check->operation != NULL)
    nuvashalauncher_task_scope_unref (check->operation);
  g_free (check);
}

static void
on_download_cancelled (GCancellable *cancellable G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncher *self = NUVASHALAUNCHER_LAUNCHER (user_data);

  nuvashalauncher_launcher_stop (self);
}

static void
begin_description_resolve (NuvashaLauncher                   *self,
                           NuvashaLauncherVersionDescription *merged,
                           const char                        *client_jar)
{
  g_autofree char *natives_dir =
    g_build_filename (self->game_directory, "natives", NULL);
  g_autofree char *title = g_strdup_printf (
    _("Minecraft %s"), self->version != NULL ? self->version : "");

  if (self->preparation.download != NULL)
    {
      nuvashalauncher_download_finish (self->preparation.download);
      g_clear_object (&self->preparation.download);
    }
  self->preparation.download = g_object_ref (
    nuvashalauncher_downloads_add (NUVASHALAUNCHER_DOWNLOAD_GAME, title));
  nuvashalauncher_download_set_detail (self->preparation.download,
                                       _("Preparing files…"));
  nuvashalauncher_download_set_progress (self->preparation.download, 0, 0);
  g_signal_connect_object (
    nuvashalauncher_download_get_cancellable (self->preparation.download),
    "cancelled", G_CALLBACK (on_download_cancelled), self, 0);

  NuvashaLauncherTaskScope *resolving = nuvashalauncher_resolve_description_async (
    merged, merged->id, client_jar, NULL, natives_dir, NULL,
    on_resolve_progress, self, on_resolved, g_object_ref (self));

  nuvashalauncher_task_scope_follow_cancellable (resolving,
                                     self->preparation.cancellable);
  nuvashalauncher_task_scope_unref_when_done (resolving);
}

static void
finish_resolve_java_check (ResolveJavaCheck *check, GPtrArray *javas)
{
  NuvashaLauncher *self = check->launcher;
  g_autofree char *instance_java = NULL;
  int required = (check->merged != NULL && check->merged->java_version != NULL)
                   ? check->merged->java_version->major_version
                   : 0;
  const NuvashaLauncherJava *java = choose_java (self, javas, required, &instance_java);

  if (java == NULL)
    {
      resolve_java_check_free (check);
      return;
    }

  remember_java (self, java);
  begin_description_resolve (self, check->merged, check->client_jar);
  resolve_java_check_free (check);
}

static void
on_resolve_custom_java_probed (GObject *source G_GNUC_UNUSED,
                               GAsyncResult *result,
                               gpointer user_data)
{
  ResolveJavaCheck *check = user_data;
  NuvashaLauncher *self = check->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherJava) custom =
    nuvashalauncher_java_probe_finish (result, &error);

  if (self->process.stop_requested ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      resolve_java_check_free (check);
      return;
    }

  if (custom == NULL)
    nuvashalauncher_instance_set_string (self->instance, "java-path", NULL, NULL);
  else
    g_ptr_array_add (check->javas, g_steal_pointer (&custom));

  finish_resolve_java_check (check, check->javas);
}

static void
on_resolve_java_detected (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result,
                          gpointer user_data)
{
  ResolveJavaCheck *check = user_data;
  NuvashaLauncher *self = check->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = nuvashalauncher_java_detect_finish (result, &error);

  if (javas == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      resolve_java_check_free (check);
      return;
    }

  if (self->process.stop_requested)
    {
      resolve_java_check_free (check);
      return;
    }

  finish_resolve_java_check (check, javas);
}

static void
start_description_resolve (NuvashaLauncher                   *self,
                           NuvashaLauncherVersionDescription *merged,
                           const char                        *client_jar)
{
  /* If Java was already selected (e.g. before a Forge/NeoForge installer),
   * go straight to resolving and downloading game files. */
  if (self->preparation.java_executable != NULL)
    {
      begin_description_resolve (self, merged, client_jar);
      return;
    }

  /* Verify a compatible Java runtime is available before downloading any
   * game files, libraries, or assets so missing-Java launches fail fast. */
  g_autofree char *instance_java = NULL;
  ResolveJavaCheck *check = g_new0 (ResolveJavaCheck, 1);

  check->launcher = g_object_ref (self);
  check->merged = g_object_ref (merged);
  check->client_jar = g_strdup (client_jar);

  g_object_get (self->instance, "java-path", &instance_java, NULL);
  if (instance_java != NULL && *instance_java != '\0')
    {
      if (g_file_test (instance_java, G_FILE_TEST_IS_REGULAR) &&
          g_file_test (instance_java, G_FILE_TEST_IS_EXECUTABLE))
        {
          check->javas = g_ptr_array_new_with_free_func (
            (GDestroyNotify) nuvashalauncher_java_free);
          check->operation = nuvashalauncher_java_probe_async (
            instance_java, on_resolve_custom_java_probed, check);
        }
      else
        {
          nuvashalauncher_instance_set_string (self->instance, "java-path", NULL, NULL);
          check->operation = nuvashalauncher_java_detect_async (
            on_resolve_java_detected, check);
        }
    }
  else
    {
      check->operation = nuvashalauncher_java_detect_async (
        on_resolve_java_detected, check);
    }

  nuvashalauncher_task_scope_follow_cancellable (check->operation,
                                                 self->preparation.cancellable);
}

/* What an install is fetching, which is a handful of files out of the
   loader's own maven: the same numbers as the resolve reports, so the
   state a screen shows is the same shape whichever is running. */
static void
on_install_progress (const char *stage, guint done, guint total,
                     gpointer user_data)
{
  NuvashaLauncher *self = NUVASHALAUNCHER_LAUNCHER (user_data);
  guint percent;
  g_autofree char *detail = NULL;

  if (self->process.stop_requested || total == 0)
    return;

  percent = (done * 100) / total;

  if (percent == self->process.last_percent)
    return;

  self->process.last_percent = percent;
  detail = g_strdup_printf ("%s %u%%", stage, percent);
  set_state (self, NUVASHALAUNCHER_LAUNCH_DOWNLOADING, detail);
}

static void
on_install_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  LoaderRequest *request = user_data;
  NuvashaLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherLoaderInstall) install = NULL;
  g_autoptr (NuvashaLauncherVersionDescription) merged = NULL;

  install = nuvashalauncher_loaders_install_finish (result, &error);

  if (install == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  if (self->process.stop_requested)
    {
      loader_request_free (request);
      return;
    }

  /* What the loader added to the game: its own document over the one of
     the Minecraft version, and the client jar its processors patched. */
  merged = nuvashalauncher_version_description_merge (request->profile, install->description);

  /* The resolve is the launch now, and it keeps itself: the request is
     about the install, which is over. */
  start_description_resolve (self, merged, install->client_jar);

  loader_request_free (request);
}

/* The Java that runs the processors of an install is the Java the game
   runs, so it is chosen once and kept for the launch itself. */
static void
on_java_for_the_install (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                         gpointer user_data)
{
  LoaderRequest *request = user_data;
  NuvashaLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = nuvashalauncher_java_detect_finish (result, &error);
  g_autofree char *instance_java = NULL;
  const NuvashaLauncherJava *java = NULL;
  int required = 0;

  if (javas == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  if (self->process.stop_requested)
    {
      loader_request_free (request);
      return;
    }

  /* The Java version the game asks for is the one the Minecraft document
     names: a loader document of its own does not say. */
  if (request->profile != NULL && request->profile->java_version != NULL)
    required = request->profile->java_version->major_version;

  java = choose_java (self, javas, required, &instance_java);

  if (java == NULL)
    {
      loader_request_free (request);
      return;
    }

  remember_java (self, java);
  log_line (self, TAG_STATE, "installing %s %s with Java %d at %s",
            nuvashalauncher_loader_display_name (request->loader), request->loader_version,
            java->major_version, java->executable);

  /* The Java answer is in: its handle is released and the install takes
   the request's one. */
  nuvashalauncher_task_scope_unref (request->operation);
  request->operation = nuvashalauncher_loaders_install_async (
    request->loader, request->game_version, request->loader_version,
    request->profile, java->executable, NULL, on_install_progress, self,
    on_install_done, request);
  nuvashalauncher_task_scope_follow_cancellable (request->operation,
                                     self->preparation.cancellable);
}

/* ---------------------------------------------------------- *
 * What the loader of an instance adds to its game
 * ---------------------------------------------------------- */

/* The document of a loader version is a version document of its own that
 * inherits from a Minecraft version, so a loader instance runs that
 * document merged over the one it inherits from: the same merge a
 * profile inherits from its parent. */
/* The document of the Minecraft version, for a loader that has to be
 * installed: the install patches its client jar, so it is the document of
 * the game that is wanted first. */
static void
on_game_document (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  LoaderRequest *request = user_data;
  NuvashaLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  NuvashaLauncherVersionDescription *document = nuvashalauncher_version_load_finish (result, &error);

  if (document == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  request->profile = document;

  if (self->process.stop_requested)
    {
      loader_request_free (request);
      return;
    }

  set_state (self, NUVASHALAUNCHER_LAUNCH_LAUNCHING, "choosing Java");

  request->operation = nuvashalauncher_java_detect_async (on_java_for_the_install, request);
  nuvashalauncher_task_scope_follow_cancellable (request->operation,
                                     self->preparation.cancellable);
}

static void
on_vanilla_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  LoaderRequest *request = user_data;
  NuvashaLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherVersionDescription) vanilla =
    nuvashalauncher_version_load_finish (result, &error);
  g_autoptr (NuvashaLauncherVersionDescription) merged = NULL;

  if (vanilla == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  merged = nuvashalauncher_version_description_merge (vanilla, request->profile);

  /* The resolve is the launch now, and it keeps itself: the request is
     about the loader profile, which is over. */
  start_description_resolve (self, merged, NULL);

  loader_request_free (request);
}

static void
on_profile_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  LoaderRequest *request = user_data;
  NuvashaLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  NuvashaLauncherVersionDescription *profile =
    nuvashalauncher_loaders_profile_finish (result, &error);

  if (profile == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  request->profile = profile;

  if (self->process.stop_requested)
    {
      loader_request_free (request);
      return;
    }

  /* The vanilla document is the next step; it takes the request's handle. */
  nuvashalauncher_task_scope_unref (request->operation);
  request->operation = nuvashalauncher_version_load_async (request->manifest,
                                              request->game_version,
                                              on_vanilla_loaded, request);
  nuvashalauncher_task_scope_follow_cancellable (request->operation,
                                     self->preparation.cancellable);
}

static void
resolve_with_a_loader (NuvashaLauncher *self,
                       NuvashaLauncherVersionManifest *manifest,
                       NuvashaLauncherLoader           loader,
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
  if (nuvashalauncher_loader_publishes_an_installer (loader))
    {
      log_line (self, TAG_STATE, "installing %s %s for %s",
                nuvashalauncher_loader_display_name (loader), loader_version,
                self->version != NULL ? self->version : "");

      request->operation = nuvashalauncher_version_load_async (manifest, request->game_version,
                                             on_game_document, request);
      nuvashalauncher_task_scope_follow_cancellable (request->operation,
                                         self->preparation.cancellable);
      return;
    }

  log_line (self, TAG_STATE, "asking %s for %s %s",
            nuvashalauncher_loader_display_name (loader), loader_version,
            self->version != NULL ? self->version : "");

  request->operation = nuvashalauncher_loaders_profile_async (loader, request->game_version,
                                                 loader_version,
                                                 on_profile_loaded, request);
  nuvashalauncher_task_scope_follow_cancellable (request->operation,
                                     self->preparation.cancellable);
}

static void
on_vanilla_only_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                        gpointer user_data)
{
  NuvashaLauncher *self = NUVASHALAUNCHER_LAUNCHER (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherVersionDescription) document =
    nuvashalauncher_version_load_finish (result, &error);

  if (document == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      g_object_unref (self);
      return;
    }

  if (self->process.stop_requested)
    {
      g_object_unref (self);
      return;
    }

  start_description_resolve (self, document, NULL);
  g_object_unref (self);
}

void
on_manifest_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  NuvashaLauncher *self = NUVASHALAUNCHER_LAUNCHER (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherVersionManifest) manifest =
    nuvashalauncher_version_manifest_fetch_finish (result, &error);

  if (manifest == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      g_object_unref (self);
      return;
    }

  if (self->process.stop_requested)
    {
      g_object_unref (self);
      return;
    }

  {
    g_autofree char *loader_version = NULL;
    NuvashaLauncherLoader loader = NUVASHALAUNCHER_LOADER_VANILLA;

    g_object_get (self->instance, "loader", &loader, "loader-version",
                  &loader_version, NULL);

    if (loader != NUVASHALAUNCHER_LOADER_VANILLA)
      {
        /* An instance that names a loader this build does not install, or
           that has no loader version, is a broken instance rather than a
           game to run: saying so is better than running the wrong thing. */
        if (!nuvashalauncher_loader_is_supported (loader))
          launcher_failed (self, _("Could not install this mod loader. Choose another version or inspect Technical Details."));
        else if (loader_version == NULL || *loader_version == '\0')
          launcher_failed (self, _("Your instance settings are invalid. Check the selected version and JVM arguments."));
        else
          resolve_with_a_loader (self, manifest, loader, loader_version);

        g_object_unref (self);
        return;
      }
  }

  {
    NuvashaLauncherTaskScope *loading = nuvashalauncher_version_load_async (
      manifest, self->version, on_vanilla_only_loaded, g_object_ref (self));

    nuvashalauncher_task_scope_follow_cancellable (loading,
                                                   self->preparation.cancellable);
    nuvashalauncher_task_scope_unref_when_done (loading);
  }

  g_object_unref (self);
}
