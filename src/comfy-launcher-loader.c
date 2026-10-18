/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-launcher-loader-private.h"
#include "comfy-auth-microsoft.h"
#include "comfy-instance-components.h"
#include "comfy-java.h"
#include "comfy-loaders.h"
#include "comfy-minecraft-resolve.h"
#include "comfy-minecraft-versions.h"

/* Internal loader-install/profile pipeline. */

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
  ComfyLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  ComfyVersionDescription *document = comfy_version_load_finish (result, &error);

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

  set_state (self, comfy_LAUNCH_LAUNCHING, "choosing Java");

  request->operation = comfy_java_detect_async (on_java_for_the_install, request);
  comfy_task_scope_follow_cancellable (request->operation,
                                     self->preparation.cancellable);
}

static void
on_vanilla_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  LoaderRequest *request = user_data;
  ComfyLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  g_autoptr (ComfyVersionDescription) vanilla =
    comfy_version_load_finish (result, &error);
  g_autoptr (ComfyVersionDescription) merged = NULL;

  if (vanilla == NULL)
    {
      launch_finished_with_error (self, g_steal_pointer (&error));
      loader_request_free (request);
      return;
    }

  merged = comfy_version_description_merge (vanilla, request->profile);

  /* The resolve is the launch now, and it keeps itself: the request is
     about the loader profile, which is over. */
  {
    g_autofree char *natives_dir =
      g_build_filename (self->game_directory, "natives", NULL);
    ComfyTaskScope *resolving = comfy_resolve_description_async (
      merged, merged->id, NULL, NULL, natives_dir, NULL, on_resolve_progress,
      self, on_resolved, g_object_ref (self));

    comfy_task_scope_follow_cancellable (resolving,
                                       self->preparation.cancellable);
    comfy_task_scope_unref_when_done (resolving);
  }

  loader_request_free (request);
}

static void
on_profile_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  LoaderRequest *request = user_data;
  ComfyLauncher *self = request->launcher;
  g_autoptr (GError) error = NULL;
  ComfyVersionDescription *profile =
    comfy_loaders_profile_finish (result, &error);

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
  comfy_task_scope_unref (request->operation);
  request->operation = comfy_version_load_async (request->manifest,
                                              request->game_version,
                                              on_vanilla_loaded, request);
  comfy_task_scope_follow_cancellable (request->operation,
                                     self->preparation.cancellable);
}

static void
resolve_with_a_loader (ComfyLauncher        *self,
                       ComfyVersionManifest *manifest,
                       ComfyLoader           loader,
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
  if (comfy_loader_publishes_an_installer (loader))
    {
      log_line (self, TAG_STATE, "installing %s %s for %s",
                comfy_loader_display_name (loader), loader_version,
                self->version != NULL ? self->version : "");

      request->operation = comfy_version_load_async (manifest, request->game_version,
                                             on_game_document, request);
      comfy_task_scope_follow_cancellable (request->operation,
                                         self->preparation.cancellable);
      return;
    }

  log_line (self, TAG_STATE, "asking %s for %s %s",
            comfy_loader_display_name (loader), loader_version,
            self->version != NULL ? self->version : "");

  request->operation = comfy_loaders_profile_async (loader, request->game_version,
                                                 loader_version,
                                                 on_profile_loaded, request);
  comfy_task_scope_follow_cancellable (request->operation,
                                     self->preparation.cancellable);
}

void
on_manifest_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  ComfyLauncher *self = comfy_LAUNCHER (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (ComfyVersionManifest) manifest =
    comfy_version_manifest_fetch_finish (result, &error);

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
    ComfyLoader loader = comfy_LOADER_VANILLA;

    g_object_get (self->instance, "loader", &loader, "loader-version",
                  &loader_version, NULL);

    if (loader != comfy_LOADER_VANILLA)
      {
        /* An instance that names a loader this build does not install, or
           that has no loader version, is a broken instance rather than a
           game to run: saying so is better than running the wrong thing. */
        if (!comfy_loader_is_supported (loader))
          launcher_failed (self, "this build of comfy does not install "
                                 "that loader");
        else if (loader_version == NULL || *loader_version == '\0')
          launcher_failed (self, "the instance has no loader version");
        else
          resolve_with_a_loader (self, manifest, loader, loader_version);

        g_object_unref (self);
        return;
      }
  }

  {
    g_autofree char *natives_dir =
      g_build_filename (self->game_directory, "natives", NULL);
    ComfyTaskScope *resolving = comfy_resolve_version_async (
      manifest, self->version, NULL, natives_dir, NULL, on_resolve_progress,
      self, on_resolved, g_object_ref (self));

    comfy_task_scope_follow_cancellable (resolving,
                                       self->preparation.cancellable);
    comfy_task_scope_unref_when_done (resolving);
  }

  g_object_unref (self);
}
