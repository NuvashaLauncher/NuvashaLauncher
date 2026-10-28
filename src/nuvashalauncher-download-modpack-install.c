/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-download-modpack-dialog-private.h"

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-operation.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-fs.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static void on_pack_downloaded (GObject *, GAsyncResult *, gpointer);
static GtkWidget *build_optional_page (nuvashalauncherDownloadModpackDialog *, GPtrArray *);
static void on_optional_cancel (GtkButton *, gpointer);
static void on_optional_install (GtkButton *, gpointer);
void start_install (nuvashalauncherDownloadModpackDialog *);
void remove_pack_file (nuvashalauncherDownloadModpackDialog *);

/* ---------------------------------------------------------- *
 * The pack fetched and the pack installed
 * ---------------------------------------------------------- */

/* References held by a background install so callbacks remain valid after
 * the dialog closes. */
typedef struct {
  nuvashalauncherDownloadModpackDialog *dialog;
  nuvashalauncherDownload *download;
  nuvashalauncherTaskScope *operation;
} InstallJob;

static void
install_job_free (InstallJob *job)
{
  g_clear_object (&job->dialog);
  g_clear_object (&job->download);
  if (job->operation != NULL)
    nuvashalauncher_operation_unref (job->operation);
  g_free (job);
}

static void
on_install_progress (guint done,
                     guint total,
  const char *path,
                     gpointer user_data)
{
  InstallJob *job = user_data;

  /* The install's "n of m files" appears in the header; a NULL
   * path marks the transition from downloading to extraction. */
  if (path == NULL)
    nuvashalauncher_download_set_detail (job->download, _("Extracting files"));
  nuvashalauncher_download_set_progress (job->download, done, total);
}

void
remove_pack_file (nuvashalauncherDownloadModpackDialog *self)
{
  if (!self->imported && self->pack_path != NULL)
    {
      nuvashalauncher_modpack_remove_archive (self->pack_path, NULL);
      g_clear_pointer (&self->pack_path, g_free);
    }
}

static void
on_install_done (GObject *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer user_data)
{
  InstallJob *job = user_data;
  nuvashalauncherDownloadModpackDialog *self = job->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) instance = NULL;

  instance = nuvashalauncher_modpack_install_finish (result, &error);

  /* The entry leaves the header's dropdown whether it worked or not, and
     the compressed pack it came from goes with it. */
  nuvashalauncher_download_finish (job->download);
  nuvashalauncher_operation_unref (job->operation);
  job->operation = NULL;
  remove_pack_file (self);

  if (instance == NULL)
    {
      /* The error page only makes sense while the dialog is up; a closed
         dialog that failed in the background just leaves the header. */
      if (!self->closed &&
          !g_error_matches (error, nuvashalauncher_MODPACK_ERROR,
                            nuvashalauncher_MODPACK_ERROR_CANCELLED) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          self->fail_is_install = TRUE;
           push_error (self, _("Could not install the modpack"), error->message,
                      G_CALLBACK (on_error_retry_install));
        }
      else if (!g_error_matches (error, nuvashalauncher_MODPACK_ERROR,
                                 nuvashalauncher_MODPACK_ERROR_CANCELLED) &&
               !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("the modpack could not be installed: %s", error->message);

      install_job_free (job);
      return;
    }

  /* The instance owns its icon. Never leave its property pointing into the
     shared cache, because cache cleanup is allowed to remove that tree. */
  if (nuvashalauncher_modpack_icon_is_regular (self->icon_path))
    {
      if (!nuvashalauncher_modpack_install_icon (self->manager, instance, self->icon_path,
                                     &error))
        if (!self->closed)
          push_error (self, _("Could not copy the instance icon"),
                      error != NULL ? error->message : _("Unknown error"),
                      G_CALLBACK (on_error_back));
    }

  install_job_free (job);
}

void
start_install (nuvashalauncherDownloadModpackDialog *self)
{
  g_autoptr (GPtrArray) chosen = NULL;
  const char *name = NULL;
  const char *directory = NULL;

  g_return_if_fail (self->modpack != NULL);

  name = self->instance_name;

  if (name == NULL || *name == '\0')
    name = self->modpack->name;

  if (name == NULL || *name == '\0')
    name = self->project != NULL && self->project->title != NULL
           ? self->project->title
           : "modpack";

  directory = self->directory; /* NULL means: the manager's default */

  /* the user's selections for the optional files. A closed dialog has
     none left to read (its switches are gone), so what the pack requires
     is what is installed. */
  chosen = g_ptr_array_new ();

  if (!self->closed && self->opt_switch_rows != NULL)
    {
      for (guint i = 0; i < self->opt_switch_rows->len; i++)
        {
          AdwSwitchRow *row = ADW_SWITCH_ROW (
            g_ptr_array_index (self->opt_switch_rows, i));

          if (adw_switch_row_get_active (row))
            {
              nuvashalauncherModpackFile *file = g_ptr_array_index (self->opt_files, i);

              g_ptr_array_add (chosen, file);
            }
        }
    }

  {
    InstallJob *job = g_new0 (InstallJob, 1);
    nuvashalauncherDownload *download = self->pack_download;

    /* The same header entry carries on from the compressed pack into the
       extract: it is one operation with two phases. */
    if (download == NULL)
      download = nuvashalauncher_downloads_add (nuvashalauncher_DOWNLOAD_MODPACK, name);

    nuvashalauncher_download_set_detail (
      download, self->modpack->files->len > 0 ? _("Downloading files")
                                              : _("Extracting files"));

    job->dialog = g_object_ref (self);
    job->download = g_object_ref (download);

    job->operation = nuvashalauncher_modpack_install_async (
      self->modpack, self->manager, name, directory, chosen,
      on_install_progress, on_install_done, job);

    /* The × on the header entry is what the user reaches for to stop this,
       so the install follows that entry's cancellable. */
    nuvashalauncher_operation_follow (job->operation,
                                       nuvashalauncher_download_get_cancellable (download));
  }
}

static GtkWidget *
build_optional_page (nuvashalauncherDownloadModpackDialog *self,
                     GPtrArray *optional_files)
{
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-optional.ui");
  AdwPreferencesGroup *heading = ADW_PREFERENCES_GROUP (
    adw_preferences_group_new ());
  GtkButton *cancel = GTK_BUTTON (gtk_builder_get_object (builder, "cancel-button"));
  GtkButton *install = GTK_BUTTON (gtk_builder_get_object (builder, "install-button"));
  GtkWidget *page = GTK_WIDGET (g_object_ref (
    gtk_builder_get_object (builder, "page")));
  AdwPreferencesPage *page_holder = ADW_PREFERENCES_PAGE (
    gtk_builder_get_object (builder, "page-holder"));

  adw_preferences_group_set_title (heading,
                                   _("Optional files"));
  adw_preferences_group_set_description (
    heading,
    _("The pack lists these files but says they are not needed. Nothing of "
      "them is installed unless you ask. Default is off"));

  g_clear_pointer (&self->opt_switch_rows, g_ptr_array_unref);
  self->opt_switch_rows = g_ptr_array_new ();
  g_clear_pointer (&self->opt_files, g_ptr_array_unref);
  self->opt_files = g_ptr_array_new_with_free_func (NULL);

  for (guint i = 0; i < optional_files->len; i++)
    {
      nuvashalauncherModpackFile *file = g_ptr_array_index (optional_files, i);
      AdwSwitchRow *row = ADW_SWITCH_ROW (adw_switch_row_new ());

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), file->path);
      adw_switch_row_set_active (ADW_SWITCH_ROW (row), FALSE);
      gtk_widget_add_css_class (GTK_WIDGET (row), "dim-label");

      adw_preferences_group_add (heading, GTK_WIDGET (row));
      g_ptr_array_add (self->opt_switch_rows, row);
      g_ptr_array_add (self->opt_files, file);
    }

  gtk_widget_add_css_class (GTK_WIDGET (install), "suggested-action");

  g_signal_connect_object (cancel, "clicked", G_CALLBACK (on_optional_cancel),
                           self, 0);
  g_signal_connect_object (install, "clicked", G_CALLBACK (on_optional_install),
                           self, 0);

  adw_preferences_page_add (page_holder, heading);

  return page;
}

static void
on_optional_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);

  dialog_details_show_again (self);
}

static void
on_optional_install (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);

  /* Pop ourselves off so the installing page is showing when
   * the install clocks in with the downloading files. */
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (self->nav));

  start_install (self);
}

/* The name the pack gives itself is what the instance follows. The name
 * field shows the Modrinth project title until the pack is read; a field
 * still holding that title, or holding nothing, is replaced by the pack's
 * own name. A name the user typed is kept exactly as typed. */
static void
adopt_pack_name (nuvashalauncherDownloadModpackDialog *self)
{
  const char *pack_name = self->modpack != NULL ? self->modpack->name : NULL;
  const char *typed = NULL;

  if (pack_name == NULL || *pack_name == '\0')
    return;

  if (self->name_entry != NULL)
    {
      typed = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));

      if (typed != NULL && *typed != '\0' &&
          g_strcmp0 (typed, self->project != NULL ? self->project->title : NULL)
            != 0)
        return;

      gtk_editable_set_text (GTK_EDITABLE (self->name_entry), pack_name);
    }

  if (self->instance_name == NULL || *self->instance_name == '\0' ||
      g_strcmp0 (self->instance_name,
                 self->project != NULL ? self->project->title : NULL) == 0)
    {
      g_free (self->instance_name);
      self->instance_name = g_strdup (pack_name);
    }
}

/* What the pack file weighs as it arrives, e.g. "312.4/500.0 MB". The
 * manager entry formats it; the dialog only hands the numbers over. */
static void
on_pack_progress (goffset bytes_done, goffset total, gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);

  if (self->pack_download != NULL)
    nuvashalauncher_download_set_bytes (self->pack_download, (guint64) bytes_done,
                            total > 0 ? (guint64) total : 0);
}

static void
on_pack_downloaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);
  g_autoptr (GError) error = NULL;

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      if (!g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR,
                            nuvashalauncher_DOWNLOAD_ERROR_CANCELLED))
        {
          /* A pack that failed after the dialog was closed has nowhere
             to say so; the reason goes to the journal and the entry
             leaves the header. */
          if (!self->closed)
            {
              self->fail_is_install = FALSE;
               push_error (self, _("Pack could not be fetched"), error->message,
                          G_CALLBACK (on_error_retry_fetch));
            }
          else
            g_warning ("the modpack \"%s\" could not be fetched: %s",
                       self->pack_path != NULL ? self->pack_path : "?",
                       error->message);
        }

      if (self->pack_download != NULL)
        nuvashalauncher_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  nuvashalauncher_operation_release (
    nuvashalauncher_modpack_new_from_zip_async (self->pack_path, on_pack_parsed,
                                    user_data),
    self->pack_download != NULL
      ? nuvashalauncher_download_get_cancellable (self->pack_download) : self->fetch);
}

void
on_pack_parsed (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  nuvashalauncherDownloadModpackDialog *self = nuvashalauncher_DOWNLOAD_MODPACK_DIALOG (user_data);
  g_autoptr (GError) error = NULL;

  g_clear_pointer (&self->modpack, nuvashalauncher_modpack_free);
  self->modpack = nuvashalauncher_modpack_new_from_zip_finish (result, &error);

  if (self->modpack == NULL)
    {
      if (!self->closed)
        {
          self->fail_is_install = FALSE;
           push_error (self, _("Invalid pack"), error->message,
                      G_CALLBACK (on_error_retry_fetch));
        }
      else
        g_warning ("the fetched modpack is not a pack: %s", error->message);

      if (self->pack_download != NULL)
        nuvashalauncher_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  if (!nuvashalauncher_modpack_resolve (self->modpack, &error))
    {
      if (!self->closed)
        {
          self->fail_is_install = FALSE;
           push_error (self, _("The pack does not define its install"),
                      error->message, G_CALLBACK (on_error_retry_fetch));
        }
      else
        g_warning ("the fetched modpack does not define its install: %s",
                   error->message);

      if (self->pack_download != NULL)
        nuvashalauncher_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  /* The pack is known now, so the instance takes its name. A closed
     dialog has no name field left; the name is kept all the same. */
  adopt_pack_name (self);

  {
    g_autoptr (GPtrArray) optional =
      nuvashalauncher_modpack_optional_client_files (self->modpack);

    /* Only a dialog that is still there can ask which optional files to
       take; a pack that arrived after it was closed installs what the
       pack requires. */
    /* A local import is intentionally headless: the dialog is never
     * presented, so there is no optional-file page for the user to answer.
     * Install the required files and overrides directly, leaving optional
     * entries out just as the closed-dialog path does. */
    if (!self->imported && !self->closed && optional->len > 0)
      {
        GtkWidget *page = build_optional_page (self, optional);

        adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                                  ADW_NAVIGATION_PAGE (page));
        operation_done (user_data);
        return;
      }
  }

  /* start_install holds its own reference from here on. Take it before
     this callback gives back the one it holds, so a dialog that is
     already closed is not freed under the install it just started. */
  start_install (self);
  operation_done (user_data);
}

gboolean
dialog_start_features (nuvashalauncherDownloadModpackDialog *self)
{
  nuvashalauncherModrinthFile *file = NULL;

  if (self->version == NULL || self->version->files == NULL)
    return FALSE;

  for (guint i = 0; i < self->version->files->len; i++)
    {
      nuvashalauncherModrinthFile *candidate = g_ptr_array_index (self->version->files, i);

      if (candidate->filename != NULL &&
          g_str_has_suffix (candidate->filename, ".mrpack"))
        {
          file = candidate;
          break;
        }
    }

  if (file == NULL)
    {
      self->fail_is_install = FALSE;
       push_error (self, _("This version has no pack file"),
                   _("It carries files that Nuvasha Launcher does not recognize."),
                  G_CALLBACK (on_error_retry_fetch));
      return FALSE;
    }

  /* The pack itself is a download like any other: it goes into the
     header's dropdown and the spinner, and its × cancels it. */
  if (self->pack_download != NULL)
    {
      nuvashalauncher_download_finish (self->pack_download);
      self->pack_download = NULL;
    }

  {
    const char *name = self->instance_name;

    if (name == NULL || *name == '\0')
      name = self->project != NULL ? self->project->title : NULL;

    if (name == NULL || *name == '\0')
      name = "modpack";

    self->pack_download = nuvashalauncher_downloads_add (nuvashalauncher_DOWNLOAD_MODPACK, name);
  }

  nuvashalauncher_download_set_detail (self->pack_download, _("Downloading Modpack"));
  g_object_add_weak_pointer (G_OBJECT (self->pack_download),
                             (gpointer *) &self->pack_download);

  {
    g_autofree char *template = g_build_filename (
      g_get_tmp_dir (), ".nuvashalauncher-pack-XXXXXX", NULL);
    int fd = g_mkstemp (template);

    if (fd < 0)
      {
        g_autoptr (GError) error = NULL;

        g_set_error (&error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "cannot create a temporary modpack file: %s",
                     g_strerror (errno));
        push_error (self, _("Pack could not be fetched"),
                    error->message, G_CALLBACK (on_error_retry_fetch));
        return FALSE;
      }

    close (fd);
    nuvashalauncher_fs_unlink (template, NULL);
    g_free (self->pack_path);
    self->pack_path = g_steal_pointer (&template);

    nuvashalauncher_operation_release (
      nuvashalauncher_download_file_async (nuvashalauncher_downloads_get_service (), file->url, self->pack_path, file->sha1,
                               G_CHECKSUM_SHA1, on_pack_progress,
                               on_pack_downloaded, g_object_ref (self)),
      nuvashalauncher_download_get_cancellable (self->pack_download));
  }

  return TRUE;
}
