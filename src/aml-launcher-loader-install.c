/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-launcher-loader-private.h"
#include "aml-java.h"
#include "aml-loaders.h"
#include "aml-minecraft-resolve.h"

void
loader_request_free (LoaderRequest *request)
{
  g_clear_object (&request->manifest);
  g_clear_object (&request->profile);
  g_clear_object (&request->launcher);

  if (request->operation != NULL)
    aml_task_scope_unref (request->operation);

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
void
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
            aml_loader_display_name (request->loader), request->loader_version,
            java->major_version, java->executable);

  aml_loaders_install_async (
    request->loader, request->game_version, request->loader_version,
    request->profile, java->executable, NULL, self->preparation.cancellable,
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

  if (self->process.stop_requested)
    {
      loader_request_free (request);
      return;
    }

  /* What the loader added to the game: its own document over the one of
     the Minecraft version, and the client jar its processors patched. */
  merged = aml_version_description_merge (request->profile, install->description);

  aml_resolve_description_async (merged, merged->id, install->client_jar, NULL,
                                 NULL, NULL, self->preparation.cancellable,
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

  if (self->process.stop_requested || total == 0)
    return;

  percent = (done * 100) / total;

  if (percent == self->process.last_percent)
    return;

  self->process.last_percent = percent;
  detail = g_strdup_printf ("%s %u%%", stage, percent);
  set_state (self, AML_LAUNCH_DOWNLOADING, detail);
}
