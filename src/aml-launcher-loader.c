/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-launcher-private.h"
#include "aml-auth-microsoft.h"
#include "aml-instance-components.h"
#include "aml-java.h"
#include "aml-loaders.h"
#include "aml-minecraft-resolve.h"
#include "aml-minecraft-versions.h"

/* Internal loader-install/profile pipeline. */

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

  java = choose_java (self, javas, required, &instance_java);

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

void
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
