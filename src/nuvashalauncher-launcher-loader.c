/* Copyright (C) 2026 Flia
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

static void
start_description_resolve (NuvashaLauncher                   *self,
                           NuvashaLauncherVersionDescription *merged,
                           const char                        *client_jar)
{
  g_autofree char *natives_dir =
    g_build_filename (self->game_directory, "natives", NULL);
  NuvashaLauncherTaskScope *resolving = nuvashalauncher_resolve_description_async (
    merged, merged->id, client_jar, NULL, natives_dir, NULL,
    on_resolve_progress, self, on_resolved, g_object_ref (self));

  nuvashalauncher_task_scope_follow_cancellable (resolving,
                                     self->preparation.cancellable);
  nuvashalauncher_task_scope_unref_when_done (resolving);
}

/* What an install is fetching, which is a handful of files out of the
   loader's own maven: the same numbers as the resolve reports, so the
   state a screen shows is the same shape whichever is running. */
static void
on_install_progress (const char *stage, guint done, guint total,
                     gpointer user_data)
{
  NuvashaLauncher *self = nuvashalauncher_LAUNCHER (user_data);
  guint percent;
  g_autofree char *detail = NULL;

  if (self->process.stop_requested || total == 0)
    return;

  percent = (done * 100) / total;

  if (percent == self->process.last_percent)
    return;

  self->process.last_percent = percent;
  detail = g_strdup_printf ("%s %u%%", stage, percent);
  set_state (self, nuvashalauncher_LAUNCH_DOWNLOADING, detail);
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

  set_state (self, nuvashalauncher_LAUNCH_LAUNCHING, "choosing Java");

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

void
on_manifest_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  NuvashaLauncher *self = nuvashalauncher_LAUNCHER (user_data);
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
    NuvashaLauncherLoader loader = nuvashalauncher_LOADER_VANILLA;

    g_object_get (self->instance, "loader", &loader, "loader-version",
                  &loader_version, NULL);

    if (loader != nuvashalauncher_LOADER_VANILLA)
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
    g_autofree char *natives_dir =
      g_build_filename (self->game_directory, "natives", NULL);
    NuvashaLauncherTaskScope *resolving = nuvashalauncher_resolve_version_async (
      manifest, self->version, NULL, natives_dir, NULL, on_resolve_progress,
      self, on_resolved, g_object_ref (self));

    nuvashalauncher_task_scope_follow_cancellable (resolving,
                                       self->preparation.cancellable);
    nuvashalauncher_task_scope_unref_when_done (resolving);
  }

  g_object_unref (self);
}
